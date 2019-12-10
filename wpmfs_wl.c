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

void fs_now_ready(struct block_device *fs_bdev) {
  _int_ctrl.fs_ready = true;
  _int_ctrl.fs_bdev = fs_bdev;
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
  return sbi->vmapi.base + (page->index << PAGE_SHIFT);
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
  blockoff = pmfs_get_block_off(sb, blocknr, PMFS_BLOCK_TYPE_4K);
  src_direct = pmfs_get_block(sb, blockoff);

  // 分配新页，新页暂未映射到 vmalloc space，因此只能使用直接映射区的地址
  errval = Allocator.pmfs_new_block(sb, &blocknr, PMFS_BLOCK_TYPE_4K, false);
  if (errval == -ENOMEM) {
    wpmfs_error("Migration(Case Vmap) failed. Memory exhausted.\n");
    return;
  }

  blockoff = pmfs_get_block_off(sb, blocknr, PMFS_BLOCK_TYPE_4K);
  dst_direct = pmfs_get_block(sb, blockoff);

  new_page = pfn_to_page(pmfs_get_pfn(sb, blockoff));
  new_page->index = page->index;
  wpmfs_mark_page(new_page, wpmfs_page_marks(new_page), wpmfs_page_marks(page));

  // 获取映射到 Tired 页的在 vmalloc space 的虚拟地址的 pte
  src_vmap = pfn_to_vaddr(sb, pfn);
  ptep = get_vmalloc_pte((unsigned long)src_vmap);
  if (!ptep) {
    wpmfs_error("Failed to get ptep.\n");
    Allocator.pmfs_free_block(sb, wpmfs_get_blocknr(sb, pfn), PMFS_BLOCK_TYPE_4K);
    return;
  }

  new_pte = mk_pte(new_page, PAGE_KERNEL);

  // 获取 mptable 表项
  mptable_slot = wpmfs_get_pgtable_slot(sb, page->index);

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

  // 检查 mptable 是否需要进行损耗均衡
  // 目前只有 kworker 单线程地使用 mptable，因此无需做并发控制
  // TODO: not tested
  if (signal_int) {
    m4m_slot_t *m4m_slot = wpmfs_get_m4m_slot(sb, page->index);

    blocknr = le64_to_cpu(m4m_slot->frag_blocknr);
    blockoff = pmfs_get_block_off(sb, blocknr, PMFS_BLOCK_TYPE_4K);
    page = pfn_to_page(pmfs_get_pfn(sb, blockoff));

    if (wpmfs_page_marks(page) & WPMFS_PAGE_TIRED) {
      src_direct = pmfs_get_block(sb, blockoff);

      // 分配新页，新页暂未映射到 vmalloc space，因此只能使用直接映射区的地址
      errval = Allocator.pmfs_new_block(sb, &blocknr, PMFS_BLOCK_TYPE_4K, false);
      if (errval == -ENOMEM) {
        wpmfs_error("Migration(Case Vmap) failed. Memory exhausted.\n");
        return;
      }
      blockoff = pmfs_get_block_off(sb, blocknr, PMFS_BLOCK_TYPE_4K);
      dst_direct = pmfs_get_block(sb, blockoff);

      memcpy_page(dst_direct, src_direct, true);
      PM_EQU(m4m_slot->frag_blocknr, cpu_to_le64(blocknr));
      pmfs_flush_buffer(&m4m_slot->frag_blocknr, sizeof(m4m_slot->frag_blocknr),
                        true);
    }
  }
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
      if (_int_ctrl.wl_switch & 0x1) _level_type_rmap(sb, pfn);
      break;

    case TYPE_VMAP:
      if (_int_ctrl.wl_switch & 0x2) _level_type_vmap(sb, pfn);
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

static int _init_int(struct super_block *sb) {
  int rc = 0;
  /* Init spinlock and kfifo */
  rc = kfifo_alloc(&_int_ctrl.fifo, sizeof(unsigned long) * (1 << 10),
                   GFP_KERNEL);
  if (rc) {
    wpmfs_error("Cannot create kfifo\n");
    return -ENOMEM;
  }

  spin_lock_init(&_int_ctrl.fifo_lock);

  // TODO: to enable multithread wear-leveling in the future.
  /* Create a workqueue to avoid causing delays for other queue users. */
  _int_ctrl.workqueue = create_singlethread_workqueue("wpmfs_wear_leveling");
  if (!_int_ctrl.workqueue) {
    wpmfs_error("Cannot create workqueue\n");
    return -ENOMEM;
  }

  /* Init work */
  INIT_WORK(&_int_ctrl.work, _int_bottom);
  return rc;
}

static int _init_mem_hard(struct super_block *sb, u64 *reserved_memory_size) {
  // 加载预分配内存
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  struct wpmfs_mptable_meta *pvmap = wpmfs_get_mptable_meta(sb);
  struct page **ppages;
  void *vmaddr;
  u64 map_size, map_pages, prealloc_memory_pages;
  u64 m4m_size, m4m_pages;
  u64 cur_slot;
  unsigned long pfn0 = sbi->phys_addr >> PAGE_SHIFT;
  int ret;
  m4m_slot_t *m4m_slot;
  mptable_slot_t *mptable;
  u64 prealloc_memory_size = *reserved_memory_size;
  INIT_TIMING(setup_vmap_time);

  PMFS_START_TIMING(setup_vmap_t, setup_vmap_time);
  // 计算映射表相关属性
  prealloc_memory_size = (prealloc_memory_size + (PAGE_SIZE - 1)) & PAGE_MASK;
  prealloc_memory_pages = prealloc_memory_size >> PAGE_SHIFT;
  map_size = prealloc_memory_pages * sizeof(mptable_slot_t);
  map_size = (map_size + (PAGE_SIZE - 1)) & PAGE_MASK;
  map_pages = map_size >> PAGE_SHIFT;

  // 计算映射表的映射表相关属性
  m4m_size = map_pages * sizeof(m4m_slot_t) + sizeof(struct wpmfs_mptable_meta);
  m4m_size = (m4m_size + (PAGE_SIZE - 1)) & PAGE_MASK;
  m4m_pages = m4m_size >> PAGE_SHIFT;

  // 最初，预分配（预留）区域对应的物理内存紧随两个映射表之后，
  // 然后线性地映射到 vmalloc space。
  PM_EQU(pvmap->num_prealloc_pages, 0);

  // 填充 m4m slots，mptable 紧随 m4m
  m4m_slot = wpmfs_get_m4m(sb, NULL);
  for (cur_slot = 0; cur_slot < map_pages; ++cur_slot) {
    PM_EQU(m4m_slot[cur_slot].frag_blocknr, cpu_to_le64(m4m_pages + cur_slot));
    // 不设置为 WPMFS_PAGE_USING，就不会对其进行损耗均衡
  }

  // 填充 pgtable slots，prealloc pages 紧随 pgtable
  // 同时登记映射到 vmalloc space 的页
  ppages =
      (struct page **)vmalloc(prealloc_memory_pages * sizeof(struct page *));
  if (!ppages) goto out_nomem;

  mptable = (mptable_slot_t *)((u8 *)pvmap + m4m_size);
  for (cur_slot = 0; cur_slot < prealloc_memory_pages; ++cur_slot) {
    u64 blocknr = m4m_pages + map_pages + cur_slot;
    struct page *page = pfn_to_page(pfn0 + blocknr);
    PM_EQU(mptable[cur_slot].blocknr, cpu_to_le64(blocknr));
    ppages[cur_slot] = page;
    wpmfs_mark_page(page, wpmfs_page_marks(page),
                    WPMFS_PAGE_VMAP | WPMFS_PAGE_USING);
    page->index = cur_slot;
  }

  // 持久化两张表，随后登记预分配内存总页数
  pmfs_flush_buffer(pvmap, m4m_size + map_size, true);
  PM_EQU(pvmap->num_prealloc_pages, cpu_to_le64(prealloc_memory_pages));
  pmfs_flush_buffer(pvmap, sizeof(struct wpmfs_mptable_meta), true);

  // map given pages to vmalloc space through vmap
  vmaddr = vmap(ppages, prealloc_memory_pages, VM_MAP, PAGE_KERNEL);
  vfree(ppages);
  if (!vmaddr) goto out_nomem;
  sbi->vmapi.base = vmaddr;
  sbi->vmapi.size = *reserved_memory_size;
  PMFS_END_TIMING(setup_vmap_t, setup_vmap_time);

  *reserved_memory_size = *reserved_memory_size + map_size + m4m_size;

  ret = 0;
  return ret;

out_nomem:
  ret = -ENOMEM;
  return ret;
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

int wpmfs_init_hard(struct super_block *sb, u64 *reserved_memory_size) {
  // TODO: to replace pmfs_init
  int errno;
  if (!_check_congfigs()) return -1;
  if (!_borrow_symbols()) return -1;

  wpmfs_init_all_cnter();
  if ((errno = _init_int(sb)) != 0) return errno;
  if ((errno = _init_mem_hard(sb, reserved_memory_size)) != 0) return errno;

  return 0;
}

static int _init_mem_soft(struct super_block *sb) {
  // 加载预分配内存
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  struct wpmfs_mptable_meta *pvmap = wpmfs_get_mptable_meta(sb);
  struct page **ppages;
  void *vmaddr;
  u64 num_m4m_slots, cur_m4m_page, cur_m4m_slot, cur_mptable_slot;
  unsigned long pfn0 = sbi->phys_addr >> PAGE_SHIFT;
  int ret;
  m4m_slot_t *m4m_base;
  int m4m_pages;
  INIT_TIMING(setup_vmap_time);

  // 同时登记映射到 vmalloc space 的页
  PMFS_START_TIMING(setup_vmap_t, setup_vmap_time);
  ppages = (struct page **)vmalloc(pvmap->num_prealloc_pages *
                                   sizeof(struct page *));
  if (!ppages) goto out_nomem;

  m4m_base = wpmfs_get_m4m(sb, &num_m4m_slots);
  m4m_pages = (round_up((unsigned long)(m4m_base + num_m4m_slots), PAGE_SIZE) -
               round_down((unsigned long)m4m_base, PAGE_SIZE)) /
              PAGE_SIZE;

  // mark m4m pages using
  for (cur_m4m_page = 0; cur_m4m_page < m4m_pages; ++cur_m4m_page) {
    unsigned long pfn = (virt_to_phys(m4m_base) >> PAGE_SHIFT) + cur_m4m_page;
    struct page *page = pfn_to_page(pfn);
    wpmfs_mark_page(page, wpmfs_page_marks(page), WPMFS_PAGE_USING);
  }

  // bookkeeping vmap pages
  for (cur_m4m_slot = 0; cur_m4m_slot < num_m4m_slots; ++cur_m4m_slot) {
    u64 frag_blocknr = le64_to_cpu(m4m_base[cur_m4m_slot].frag_blocknr);
    u64 frag_blockoff = pmfs_get_block_off(sb, frag_blocknr, 0);
    mptable_slot_t *frag_base =
        (mptable_slot_t *)pmfs_get_block(sb, frag_blockoff);
    struct page *frag_page = pfn_to_page(virt_to_phys(frag_base) >> PAGE_SHIFT);

    // mark mptable pages using
    wpmfs_mark_page(frag_page, wpmfs_page_marks(frag_page), WPMFS_PAGE_USING);
    for (cur_mptable_slot = 0; cur_mptable_slot < frag_mptable_slots();
         ++cur_mptable_slot) {
      pgoff_t index = cur_m4m_slot * frag_mptable_slots() + cur_mptable_slot;
      u64 blocknr = le64_to_cpu(frag_base[cur_mptable_slot].blocknr);
      struct page *page = pfn_to_page(pfn0 + blocknr);

      if (unlikely(index >= pvmap->num_prealloc_pages)) break;
      ppages[index] = page;
      wpmfs_mark_page(page, wpmfs_page_marks(page),
                      WPMFS_PAGE_VMAP | WPMFS_PAGE_USING);
      page->index = index;
    }
  }

  // map given pages to vmalloc space through vmap
  vmaddr = vmap(ppages, pvmap->num_prealloc_pages, VM_MAP, PAGE_KERNEL);
  vfree(ppages);
  if (!vmaddr) goto out_nomem;
  sbi->vmapi.base = vmaddr;
  sbi->vmapi.size = pvmap->num_prealloc_pages * PAGE_SIZE;
  PMFS_END_TIMING(setup_vmap_t, setup_vmap_time);

  ret = 0;
  return ret;

out_nomem:
  ret = -ENOMEM;
  return ret;
}

int wpmfs_init_soft(struct super_block *sb) {
  int errno;
  if (!_check_congfigs()) return -1;
  if (!_borrow_symbols()) return -1;

  if ((errno = _init_int(sb)) != 0) return errno;
  if ((errno = _init_mem_soft(sb)) != 0) return errno;

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
  vunmap(sbi->vmapi.base);
}

void wpmfs_exit(struct super_block *sb) {
  _exit_int();
  _exit_mem(sb);
}

void wpmfs_print_wl_switch(struct super_block *sb) {
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

void wpmfs_print_memory_layout(struct super_block *sb,
                               unsigned long reserved_size) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  struct pmfs_super_block *super = pmfs_get_super(sb);
  struct pmfs_inode *inode_table = pmfs_get_inode_table(sb);
  pmfs_journal_t *journal_meta = pmfs_get_journal(sb);
  void *journal_data = sbi->journal_base_addr;
  unsigned long num_reserved_block =
      (reserved_size + sb->s_blocksize - 1) >> sb->s_blocksize_bits;
  uint64_t datablk_off = pmfs_get_block_off(
      sb, sbi->block_start + num_reserved_block, PMFS_BLOCK_TYPE_4K);
  void *pdatablk = pmfs_get_block(sb, datablk_off);

  wpmfs_assert(wpmfs_get_vblock(sb, journal_meta->base) == journal_data);
  wpmfs_assert(cpu_to_le32(journal_meta->size) == sbi->jsize);

  pmfs_info("The memory layout of wpmfs:\n");

  pmfs_info("Memory reserved: %lu.\n", reserved_size);
  pmfs_info("Memory reserved for prealloc pages: %llu.\n", sbi->vmapi.size);

  wpmfs_assert(is_vmalloc_addr(super));
  pmfs_info("Superblock - start at 0x%px, len %lu.\n", super,
            sizeof(struct pmfs_super_block));
  wpmfs_assert(is_vmalloc_addr(inode_table));
  pmfs_info("Inode table - start at 0x%px, len %lu.\n", inode_table,
            sizeof(struct pmfs_inode));
  wpmfs_assert(is_vmalloc_addr(journal_meta));
  pmfs_info("Journal meta - start at 0x%px, len %lu.\n", journal_meta,
            sizeof(pmfs_journal_t));
  wpmfs_assert(is_vmalloc_addr(journal_data));
  pmfs_info("Journal data - start at 0x%px, len %d.\n", journal_data,
            cpu_to_le32(journal_meta->size));

  pmfs_info("datablks - starts at 0x%px.\n", pdatablk);
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
