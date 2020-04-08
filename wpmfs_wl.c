#include "wpmfs_wl.h"

#include <asm/smp.h>
#include <asm/tlbflush.h>
#include <linux/dax.h>
#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/kfifo.h>
#include <linux/migrate.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include "pmfs.h"
#include "wpmfs_wt.h"

// https://elixir.bootlin.com/linux/v4.19.49/source/fs/dax.c#L70
#define RADIX_DAX_SHIFT (RADIX_TREE_EXCEPTIONAL_SHIFT + 4)
#define RADIX_DAX_ENTRY_LOCK (1 << RADIX_TREE_EXCEPTIONAL_SHIFT)
#define RADIX_DAX_PMD (1 << (RADIX_TREE_EXCEPTIONAL_SHIFT + 1))
#define RADIX_DAX_ZERO_PAGE (1 << (RADIX_TREE_EXCEPTIONAL_SHIFT + 2))
#define RADIX_DAX_EMPTY (1 << (RADIX_TREE_EXCEPTIONAL_SHIFT + 3))

static int dax_is_pte_entry(void *entry) {
  return !((unsigned long)entry & RADIX_DAX_PMD);
}

enum PAGE_TYPE { TYPE_RMAP, TYPE_VMAP, TYPE_STRANDED };

bool (*ppage_vma_mapped_walk)(struct page_vma_mapped_walk *);
void (*prmap_walk)(struct page *, struct rmap_walk_control *);

/**
 * struct int_ctrl - the interrupt controller
 * @fs_ready: if fs was mounted successfully
 * @fifo: a queue for storing pfn
 * @fifo_lock: protect access to pfn queue
 * @work: should bind to the bottom half
 * @workqueue: a private workqueue run works
 * @wl_switch: switch of wear-leveling mechanism.
 *             1st bit for rmap
 *             2nd bit for vmap
 *             3rd bit for stranded
 *
 * This struct provides communication channels for the top half, to temporally
 * store pfns of hot pages and shedule the bottom half.
 */
struct int_ctrl {
  bool fs_ready;
  struct block_device *fs_bdev;

  struct kfifo fifo;
  spinlock_t fifo_lock;

  struct work_struct work;
  struct workqueue_struct *workqueue;

  int wl_switch;
};

struct int_ctrl _int_ctrl = {
    .fs_ready = false,
    .fs_bdev = NULL,
    .workqueue = NULL,
};

static void wpmfs_print_wl_switch(struct super_block *sb) {
  if (!_int_ctrl.wl_switch) {
    pmfs_info("Wear-leveling mechanism: Disabled.\n");
    return;
  }

  if (_int_ctrl.wl_switch & 0x1)
    pmfs_info("Wear-leveling mechanism: Rmap eabled.\n");
  if (_int_ctrl.wl_switch & 0x2)
    pmfs_info("Wear-leveling mechanism: Vmap enable.\n");
  if (_int_ctrl.wl_switch & 0x4)
    pmfs_info("Wear-leveling mechanism: Stranded enabled.\n");
}

static void wpmfs_print_memory_layout(struct super_block *sb) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);

  pmfs_info(
      "The memory layout of WellPM "
      "(fmt: Start addr, size, VM area):\n");

  pmfs_info("0x%px, %lu, %s.\n", sbi->virt_addr, sbi->initsize, "VM_X");
  pmfs_info("0x%px, %llu, %s.\n", sbi->vmapi.base_static,
            sbi->vmapi.size_static, "VM_S");
  pmfs_info("0x%px, %llu / %llu, %s.\n", sbi->vmapi.base_dynamic,
            sbi->vmapi.size_dynamic, (u64)(1024 * 1024 * 1024), "VM_D");
}

void fs_now_ready(struct super_block *sb) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  struct block_device *fs_bdev = sbi->s_bdev;

  _int_ctrl.fs_ready = true;
  _int_ctrl.fs_bdev = fs_bdev;

  wpmfs_print_memory_layout(sb);
  wpmfs_print_wl_switch(sb);
}

void fs_now_removed(void) {
  _int_ctrl.fs_ready = false;
  _int_ctrl.fs_bdev = NULL;
}

void wpmfs_set_wl_switch(int wlsw) { _int_ctrl.wl_switch = wlsw; }
bool wpmfs_wl_stranded_enabled(void) { return _int_ctrl.wl_switch & 0x4; }

static bool _fetch_int_requests(unsigned long *ppfn) {
  // 这里使用最简单的请求调度算法，不对页迁移请求进行任何合并操作
  return kfifo_out_spinlocked(&_int_ctrl.fifo, ppfn, sizeof(*ppfn),
                              &_int_ctrl.fifo_lock);
}

// 设置了 rmap 的页，即为映射到进程地址空间的页
static inline bool _check_rmap(unsigned long pfn) {
  struct page *page = pfn_to_page(pfn);
  return page_mapping(page);
}

static bool _check_vmap(unsigned long pfn) {
  struct page *page = pfn_to_page(pfn);
  return wpmfs_page_marks(page) & WPMFS_PAGE_VMAP;
}

static enum PAGE_TYPE _page_type(unsigned long pfn) {
  if (_check_rmap(pfn))
    return TYPE_RMAP;
  else if (_check_vmap(pfn))
    return TYPE_VMAP;
  else
    return TYPE_STRANDED;
}

/*
 * https://elixir.bootlin.com/linux/v4.19.49/source/fs/dax.c#L187
 * Check whether the given slot is locked.  Must be called with the i_pages
 * lock held.
 */
static inline int slot_locked(struct address_space *mapping, void **slot) {
  unsigned long entry = (unsigned long)radix_tree_deref_slot_protected(
      slot, &mapping->i_pages.xa_lock);
  return entry & RADIX_DAX_ENTRY_LOCK;
}

/*
 * https://elixir.bootlin.com/linux/v4.19.49/source/fs/dax.c#L197
 * Mark the given slot as locked.  Must be called with the i_pages lock held.
 */
static inline void *lock_slot(struct address_space *mapping, void **slot) {
  unsigned long entry = (unsigned long)radix_tree_deref_slot_protected(
      slot, &mapping->i_pages.xa_lock);

  entry |= RADIX_DAX_ENTRY_LOCK;
  radix_tree_replace_slot(&mapping->i_pages, slot, (void *)entry);
  return (void *)entry;
}

/*
 * https://elixir.bootlin.com/linux/v4.19.49/source/fs/dax.c#L210
 * Mark the given slot as unlocked.  Must be called with the i_pages lock held.
 */
static inline void *unlock_slot(struct address_space *mapping, void **slot) {
  unsigned long entry = (unsigned long)radix_tree_deref_slot_protected(
      slot, &mapping->i_pages.xa_lock);

  entry &= ~(unsigned long)RADIX_DAX_ENTRY_LOCK;
  radix_tree_replace_slot(&mapping->i_pages, slot, (void *)entry);
  return (void *)entry;
}

static void *trygrab_mapping_entry(struct address_space *mapping, pgoff_t index,
                                   void __rcu ***slotp) {
  void *entry;
  void __rcu **slot;

  xa_lock_irq(&mapping->i_pages);
  slot = radix_tree_lookup_slot(&mapping->i_pages, index);

  if (unlikely(!slot) || slot_locked(mapping, slot)) {
    xa_unlock_irq(&mapping->i_pages);
    wpmfs_dbg_wl_rmap(
        "Migration(Case Rmap) failed. Cannot grab mapping entry.\n");
    return ERR_PTR(-EAGAIN);
  }

  entry = lock_slot(mapping, slot);
  xa_unlock_irq(&mapping->i_pages);
  if (slotp) *slotp = slot;
  return entry;
}

// https://elixir.bootlin.com/linux/v4.19.49/source/fs/dax.c#L290
static void put_locked_mapping_entry(struct address_space *mapping,
                                     pgoff_t index) {
  void **slot;

  xa_lock_irq(&mapping->i_pages);
  slot = radix_tree_lookup_slot(&mapping->i_pages, index);
  if (unlikely(!slot)) {
    xa_unlock_irq(&mapping->i_pages);
    wpmfs_dbg_wl_rmap(
        "Migration(Case Rmap) failed. Cannot lock mapping entry.\n");
    return;
  }
  unlock_slot(mapping, slot);
  xa_unlock_irq(&mapping->i_pages);
}

// https://elixir.bootlin.com/linux/v4.19.49/source/fs/dax.c#L81
static void *dax_radix_locked_entry(unsigned long pfn, unsigned long flags) {
  return (void *)(RADIX_TREE_EXCEPTIONAL_ENTRY | flags |
                  (pfn << RADIX_DAX_SHIFT) | RADIX_DAX_ENTRY_LOCK);
}

static void *dax_radix_unlocked_entry(unsigned long pfn, unsigned long flags) {
  return (void *)((RADIX_TREE_EXCEPTIONAL_ENTRY | flags |
                   (pfn << RADIX_DAX_SHIFT)) &
                  ~RADIX_DAX_ENTRY_LOCK);
}

static void replace_slot(struct address_space *mapping, pgoff_t index,
                         void *unloked_new_entry) {
  void __rcu **slot;
  void *entry;
  extern void dax_wake_mapping_entry_waiter(struct address_space * mapping,
                                            pgoff_t index, void *entry,
                                            bool wake_all);

  xa_lock_irq(&mapping->i_pages);
  slot = radix_tree_lookup_slot(&mapping->i_pages, index);
  entry = radix_tree_deref_slot_protected(slot, &mapping->i_pages.xa_lock);
  if (!((unsigned long)entry & RADIX_DAX_ENTRY_LOCK)) {
    wpmfs_error("Fatal! Slot being replaced is not locked.\n");
  }
  radix_tree_replace_slot(&mapping->i_pages, slot, unloked_new_entry);
  xa_unlock_irq(&mapping->i_pages);
  // export_symbol before using.
  // https://elixir.bootlin.com/linux/v4.19.49/source/fs/dax.c#L165
  // to save applications from sleeping forever.
  dax_wake_mapping_entry_waiter(mapping, index, entry, false);
}

static int _level_type_rmap_core(struct page *page, struct page *new_page,
                                 unsigned long blocknr) {
  struct address_space *mapping = page_mapping(page);
  struct inode *inode = mapping->host;
  pgoff_t pgoff = page_index(page);
  unsigned long new_pfn = pfn_t_to_pfn(page_to_pfn_t(new_page));
  unsigned long flags;
  void *entry, *new_entry;
  void __rcu **slot;

  if (PageTransHuge(page)) {
    wpmfs_error("Huge page is not supported.\n");
    return -EAGAIN;
  }

  entry = trygrab_mapping_entry(mapping, pgoff, &slot);
  if (IS_ERR(entry)) return -EAGAIN;

  new_page->lru = page->lru;
  new_page->index = page->index;
  new_page->mapping = page->mapping;
  flags = (unsigned long)entry & ((1 << RADIX_DAX_SHIFT) - 1);
  new_entry = dax_radix_unlocked_entry(new_pfn, flags);

  unmap_mapping_range(mapping, pgoff << PAGE_SHIFT, PAGE_SIZE, false);
  page->mapping = NULL;
  page->index = 0;

  wpmfs_replace_single_datablk(inode, pgoff, blocknr);
  replace_slot(mapping, pgoff, new_entry);

  return MIGRATEPAGE_SUCCESS;
}

static void _level_type_rmap(struct super_block *sb, unsigned long pfn) {
  unsigned long blocknr;
  u64 blockoff;
  struct page *page = pfn_to_page(pfn);
  struct page *new_page = NULL;
  struct address_space *mapping;
  struct inode *inode;
  int errval;

  // Since the mapping(truncate) may be cleared, and the inode(iput) may
  // be dropped, we need to do something to ensure grab the both the mapping
  // and inode.

  // To serialize `evict_inode`, by the way, `destroy_inode`.
  mutex_lock(&PMFS_SB(sb)->inode_table_mutex);

  // Get a copy of mapping.
  // Since mapping is just a pointer pointing to i_datas,
  // we are always to access mapping unless the whole inode is freed,
  // which is impossible due to the serialization of `destroy_inode`.
  if (!(mapping = page_mapping(page))) {
    mutex_unlock(&PMFS_SB(sb)->inode_table_mutex);
    return;
  }

  inode = mapping->host;

  // inode may be freeing,
  // or the page has already been truncated before grabing the inode.
  if (!(inode = igrab(inode))) {
    mutex_unlock(&PMFS_SB(sb)->inode_table_mutex);
    return;
  }

  if (!page_mapping(page)) {
    iput(inode);
    mutex_unlock(&PMFS_SB(sb)->inode_table_mutex);
    return;
  }

  mutex_unlock(&PMFS_SB(sb)->inode_table_mutex);

  errval = Allocator.pmfs_new_block(sb, &blocknr, PMFS_BLOCK_TYPE_4K, false);
  if (errval == -ENOMEM) {
    wpmfs_error("Migration(Case Rmap) failed. Memory exhausted.\n");
    iput(inode);
    return;
  }

  blockoff = pmfs_get_block_off(sb, blocknr, PMFS_BLOCK_TYPE_4K);
  new_page = pfn_to_page(pmfs_get_pfn(sb, blockoff));

  // 加读锁，因为页迁移过程是安全的，而且不影响读者所见结果
  inode_lock_shared(inode);

  errval = _level_type_rmap_core(page, new_page, blocknr);
  if (errval == -EAGAIN) {
    wpmfs_dbg_wl_rmap("Migration(Case Rmap) failed. Contention occured.\n");
    inode_unlock_shared(inode);
    iput(inode);

    Allocator.pmfs_free_block(sb, blocknr, PMFS_BLOCK_TYPE_4K);
    return;
  } else {
    blocknr = wpmfs_get_blocknr(sb, pfn);
    Allocator.pmfs_free_block(sb, blocknr, PMFS_BLOCK_TYPE_4K);
  }

  inode_unlock_shared(inode);
  wpmfs_dbg_wl_rmap("Migration(Case Rmap) succeed. pgoff = %lu.\n",
                    new_page->index);

  iput(inode);
}

static void *pfn_to_vaddr(struct super_block *sb, unsigned long pfn) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  struct page *page = pfn_to_page(pfn);

  PMFS_ASSERT(wpmfs_page_marks(page) & WPMFS_PAGE_VMAP);
  return (wpmfs_page_marks(page) & WPMFS_PAGE_VMAP_DYNAMIC)
             ? sbi->vmapi.base_dynamic + (page->index << PAGE_SHIFT)
             : sbi->vmapi.base_static + (page->index << PAGE_SHIFT);
}

static pte_t *get_vmalloc_pte(unsigned long address) {
  pgd_t *base, *pgd;
  p4d_t *p4d;
  pud_t *pud;
  pmd_t *pmd;
  pte_t *pte;
  unsigned long tmp;

  PMFS_ASSERT(is_vmalloc_addr((void *)address));

  // pre-fault addr, atomic instruction acts as membar
  tmp = atomic_long_read((atomic64_t *)address);

  base = __va(read_cr3_pa());
  pgd = &base[pgd_index(address)];
  if (!pgd_present(*pgd)) return NULL;

  p4d = p4d_offset(pgd, address);
  if (!p4d_present(*p4d)) return NULL;

  pud = pud_offset(p4d, address);
  if (!pud_present(*pud)) return NULL;

  pmd = pmd_offset(pud, address);
  if (!pmd_present(*pmd)) return NULL;

  pte = pte_offset_kernel(pmd, address);
  if (!pte_present(*pte)) return NULL;

  return pte;
}

static void flush_kernel_pte(void *info) {
  __flush_tlb_one_kernel((unsigned long)info);
}

static void _level_type_vmap(struct super_block *sb, unsigned long pfn) {
  unsigned long blocknr, blockoff;
  struct page *page = pfn_to_page(pfn), *new_page;
  int errval;
  void *src_direct, *dst_direct, *src_vmap;
  pte_t *ptep, new_pte;
  mptable_slot_t *mptable_slot;
  bool signal_int;

  // 获取 Tired 页，使用直接映射区的虚拟地址，而非 vmalloc space 的虚拟地址
  // 因为后续要对后者设置为只读，这对前者并无影响
  blocknr = wpmfs_get_blocknr(sb, pfn);
  blockoff = wpmfs_get_blockoff(sb, blocknr, 0).blockoff;
  src_direct = pmfs_get_block(sb, blockoff);

  // 分配新页，新页暂未映射到 vmalloc space，因此只能使用直接映射区的地址
  errval = Allocator.pmfs_new_block(sb, &blocknr, PMFS_BLOCK_TYPE_4K, false);
  if (errval == -ENOMEM) {
    wpmfs_error("Migration(Case Vmap) failed. Memory exhausted.\n");
    return;
  }

  blockoff = wpmfs_get_blockoff(sb, blocknr, 0).blockoff;
  dst_direct = pmfs_get_block(sb, blockoff);

  new_page = pfn_to_page(pmfs_get_pfn(sb, blockoff));
  new_page->index = page->index;
  wpmfs_mark_page(new_page, wpmfs_page_marks(new_page), wpmfs_page_marks(page));

  // 获取映射到 Tired 页的在 vmalloc space 的虚拟地址的 pte
  src_vmap = pfn_to_vaddr(sb, pfn);
  ptep = get_vmalloc_pte((unsigned long)src_vmap);
  if (!ptep) {
    wpmfs_error("Failed to get ptep.\n");
    Allocator.pmfs_free_block(sb, wpmfs_get_blocknr(sb, pfn),
                              PMFS_BLOCK_TYPE_4K);
    return;
  }

  new_pte = mk_pte(new_page, PAGE_KERNEL);

  // 获取 mptable 表项
  mptable_slot = (mptable_slot_t *)wpmfs_reget_mptable_slot(sb, page);

  // copy the page content
  // 此处引入了新型的页保护错误，所以我们在 page.c 中相应地添加了一条新路径
  // less likely, but necessary
  preempt_disable();
  set_pte_atomic(ptep, pte_wrprotect(*ptep));
  on_each_cpu(flush_kernel_pte, src_vmap, 1);

  wpmfs_dbg_wl_vmap("src, head = %u, tail = %u.\n",
                    le32_to_cpu(*(__le32 *)(src_direct + 140)),
                    le32_to_cpu(*(__le32 *)(src_direct + 144)));
  memcpy_page(dst_direct, src_direct, false);
  PM_EQU_NO_INT(mptable_slot->blocknr, cpu_to_le64(blocknr), signal_int);
  wpmfs_assert(signal_int == false);
  pmfs_flush_buffer(&mptable_slot->blocknr, sizeof(mptable_slot->blocknr),
                    true);
  wpmfs_dbg_wl_vmap("src, head = %u, tail = %u.\n",
                    le32_to_cpu(*(__le32 *)(src_direct + 140)),
                    le32_to_cpu(*(__le32 *)(src_direct + 144)));
  wpmfs_dbg_wl_vmap("dst, head = %u, tail = %u.\n",
                    le32_to_cpu(*(__le32 *)(dst_direct + 140)),
                    le32_to_cpu(*(__le32 *)(dst_direct + 144)));

  new_pte = pte_mkwrite(new_pte);
  set_pte_atomic(ptep, new_pte);
  on_each_cpu(flush_kernel_pte, src_vmap, 1);
  preempt_enable();

  Allocator.pmfs_free_block(sb, wpmfs_get_blocknr(sb, pfn), PMFS_BLOCK_TYPE_4K);

  wpmfs_dbg_wl_vmap("Migration(Case Vmap) succeed. %px: %px -> %px. cpu: %d\n",
                    src_vmap, src_direct, dst_direct, raw_smp_processor_id());
}

static void _level_type_stranded(struct super_block *sb, unsigned long pfn) {
  // just mark the page tired.
  struct page *page = pfn_to_page(pfn);
  unsigned long page_marks = wpmfs_page_marks(page);
  if (!(page_marks & WPMFS_PAGE_USING)) return;

  wpmfs_mark_page(page, page_marks, page_marks | WPMFS_PAGE_TIRED);
}

static void _wear_lerveling(struct super_block *sb, unsigned long pfn) {
  switch (_page_type(pfn)) {
    case TYPE_RMAP:
      if (_int_ctrl.wl_switch & 0x1) {
        INIT_TIMING(wl_rmap_time);
        PMFS_START_TIMING(wl_rmap_t, wl_rmap_time);
        _level_type_rmap(sb, pfn);
        PMFS_END_TIMING(wl_rmap_t, wl_rmap_time);
        break;
      }

    case TYPE_VMAP:
      if (_int_ctrl.wl_switch & 0x2) {
        INIT_TIMING(wl_vmap_time);
        PMFS_START_TIMING(wl_vmap_t, wl_vmap_time);
        _level_type_vmap(sb, pfn);
        PMFS_END_TIMING(wl_vmap_t, wl_vmap_time);
      }
      break;

    case TYPE_STRANDED:
      if (_int_ctrl.wl_switch & 0x4) _level_type_stranded(sb, pfn);
      break;

    default:
      wpmfs_assert(0);
      break;
  }
}

static void _int_bottom(struct work_struct *work) {
  unsigned long pfn;
  struct super_block *sb;

  sb = get_super(_int_ctrl.fs_bdev);
  if (!sb) {
    wpmfs_error("Cannot get super block\n");
    return;
  }

  while (_fetch_int_requests(&pfn)) {
    if (wpmfs_page_marks(pfn_to_page(pfn)) & WPMFS_PAGE_USING)
      _wear_lerveling(sb, pfn);
  }

  drop_super(sb);
}

void wpmfs_int_top(unsigned long pfn) {
  /* Check if filesytem has been established */
  if (!_int_ctrl.fs_ready) return;
  wpmfs_assert(_int_ctrl.workqueue);

  if (!_int_ctrl.wl_switch) return;

  /* Enqueue to the FIFO */
  if (kfifo_in_spinlocked(&_int_ctrl.fifo, &pfn, sizeof(pfn),
                          &_int_ctrl.fifo_lock))
    /* Schedule the bottom handler */
    queue_work(_int_ctrl.workqueue, &_int_ctrl.work);
  else
    wpmfs_error(
        "kfifo for workqueue has full. pfn intended to insert is %lu.\n", pfn);
}

static bool _init_int(struct super_block *sb) {
  int succ = false;

  /* Init spinlock and kfifo */
  if (kfifo_alloc(&_int_ctrl.fifo, sizeof(unsigned long) * (1 << 10),
                  GFP_KERNEL)) {
    wpmfs_error("Cannot create kfifo\n");
    kfifo_free(&_int_ctrl.fifo);
    goto out_fail;
  }

  spin_lock_init(&_int_ctrl.fifo_lock);

  /* Create a workqueue to avoid disturbing other queue users. */
  _int_ctrl.workqueue = create_singlethread_workqueue("wpmfs_wear_leveling");
  if (!_int_ctrl.workqueue) {
    wpmfs_error("Cannot create workqueue\n");
    goto out_fail;
  }

  /* Init work */
  INIT_WORK(&_int_ctrl.work, _int_bottom);
  succ = true;

out_fail:
  return succ;
}

static void *_borrow_symbol(char *sym_name) {
  void *tgt = (void *)kallsyms_lookup_name(sym_name);
  if (!tgt) wpmfs_error("Symbol %s unfound\n", sym_name);
  return tgt;
}

static bool _borrow_symbols(void) {
  ppage_vma_mapped_walk = _borrow_symbol("page_vma_mapped_walk");
  if (!ppage_vma_mapped_walk) return false;

  prmap_walk = _borrow_symbol("rmap_walk");
  if (!prmap_walk) return false;

  return true;
}

static bool _check_congfigs(void) {
  if (!IS_ENABLED(CONFIG_MIGRATION)) {
    wpmfs_error("Config %s undefined.\n", "CONFIG_MIGRATION");
    return false;
  }

  return true;
}

int wpmfs_init(struct super_block *sb, u64 *reserved_memory_size) {
  if (!_check_congfigs()) return -EPERM;
  if (!_borrow_symbols()) return -EPERM;

  wpmfs_init_all_cnter();

  if (!_init_int(sb)) return -ENOMEM;
  if (!wpmfs_setup_memory(sb, reserved_memory_size)) return -ENOMEM;

  return 0;
}

int wpmfs_recv(struct super_block *sb) {
  if (!_check_congfigs()) return -EPERM;
  if (!_borrow_symbols()) return -EPERM;

  if (!_init_int(sb)) return -ENOMEM;
  if (!wpmfs_recv_memory(sb)) return -ENOMEM;

  return 0;
}

static void _exit_int(void) {
  /* Safely destroy a workqueue. */
  if (_int_ctrl.workqueue) {
    unsigned long lenq = kfifo_len(&_int_ctrl.fifo) / sizeof(unsigned long);
    if (lenq) wpmfs_error("kfifo remains stained. queue length = %lu.\n", lenq);

    destroy_workqueue(_int_ctrl.workqueue);
  }
  kfifo_free(&_int_ctrl.fifo);
}

static void _exit_mem(struct super_block *sb) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  vunmap(sbi->vmapi.base_static);
  vunmap(sbi->vmapi.base_dynamic);
}

void wpmfs_exit(struct super_block *sb) {
  _exit_int();
  _exit_mem(sb);
}

u64 wpmfs_get_capacity(void) {
  u64 capacity;
  struct super_block *sb;

  sb = get_super(_int_ctrl.fs_bdev);
  if (!sb) {
    wpmfs_error("Cannot get super block\n");
    return 0;
  }

  capacity = PMFS_SB(sb)->initsize;
  drop_super(sb);

  return capacity;
}

bool wpmfs_get_fs_wear(unsigned long blocknr, u64 *wear_times) {
  static u64 capacity = 0;

  if (!capacity) capacity = wpmfs_get_capacity();
  if (blocknr >= (capacity >> PAGE_SHIFT)) return false;

  *wear_times = wt_cnter_read_blocknr(blocknr);
  return true;
}
