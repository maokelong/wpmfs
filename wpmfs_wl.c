#include "wpmfs_wl.h"
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
  // TODO:
  return false;
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
    wpmfs_dbg_wl_rmap("Failed to grab mapping entry.\n");
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
  void *entry, **slot;

  xa_lock_irq(&mapping->i_pages);
  slot = radix_tree_lookup_slot(&mapping->i_pages, index);
  if (unlikely(!slot)) {
    xa_unlock_irq(&mapping->i_pages);
    wpmfs_dbg_wl_rmap("Failed to lock mapping entry.\n");
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
  extern void dax_wake_mapping_entry_waiter(struct address_space * mapping,
                                            pgoff_t index, void *entry,
                                            bool wake_all);
  //  otherwise applications may sleep forever.
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

  errval = pmfs_new_block(sb, &blocknr, PMFS_BLOCK_TYPE_4K, false);
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

    pmfs_free_block(sb, blocknr, PMFS_BLOCK_TYPE_4K);
    return;
  } else {
    blocknr = wpmfs_get_blocknr(sb, pfn);
    pmfs_free_block(sb, blocknr, PMFS_BLOCK_TYPE_4K);
  }

  inode_unlock_shared(inode);
  wpmfs_dbg_wl_rmap("Migration(Case Rmap) succeed. pgoff = %lu.\n",
                    new_page->index);

  wpmfs_file_updated(inode, true);
  iput(inode);
}

static void _level_type_vmap(struct super_block *sb, unsigned long pfn) {
  // TODO
  wpmfs_error("impossible routine currently.\n");
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
      _level_type_rmap(sb, pfn);
      break;

    case TYPE_VMAP:
      _level_type_vmap(sb, pfn);
      break;

    case TYPE_STRANDED:
      _level_type_stranded(sb, pfn);
      break;

    default:
      wpmfs_assert(0);
      break;
  }
}

static void _int_bottom(struct work_struct *work) {
  unsigned long pfn;
  struct super_block *sb;

  if (!_fetch_int_requests(&pfn)) {
    wpmfs_error("Fetch from empty queue.\n");
    return;
  }

  sb = get_super(_int_ctrl.fs_bdev);
  if (!sb) {
    wpmfs_error("Cannot get super block\n");
    return;
  }

  if (wpmfs_page_marks(pfn_to_page(pfn)) & WPMFS_PAGE_USING)
    _wear_lerveling(sb, pfn);

  drop_super(sb);
}

void wpmfs_int_top(unsigned long pfn) {
  /* Check if filesytem has been established */
  if (!_int_ctrl.fs_ready) return;
  wpmfs_assert(_int_ctrl.workqueue);

  /* Enqueue to the FIFO */
  if (kfifo_in_spinlocked(&_int_ctrl.fifo, &pfn, sizeof(pfn),
                          &_int_ctrl.fifo_lock))
    /* Schedule the bottom handler */
    queue_work(_int_ctrl.workqueue, &_int_ctrl.work);
  else
    wpmfs_error("kfifo may be full.\n");
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
  _int_ctrl.workqueue = create_singlethread_workqueue("my_workqueue");
  if (!_int_ctrl.workqueue) {
    wpmfs_error("Cannot create workqueue\n");
    return -ENOMEM;
  }

  /* Init work */
  INIT_WORK(&_int_ctrl.work, _int_bottom);
  return rc;
}

static int _init_mem(struct super_block *sb, u64 static_area_size) {
  // 加载静态区内存
  // TODO: 目前只支持硬重启，而且只有一级映射
  struct pmfs_sb_info *psbi = PMFS_SB(sb);
  wpmfs_vmap_t *pvmap = wpmfs_get_vmap(sb, 0);
  wpmfs_vmap_entry_t *vmap_entry0;
  struct page **ppages;
  void *vmaddr;
  u64 map_size, map_pages, static_area_pages;
  u64 cur_page, level;
  unsigned long pfn0 = psbi->phys_addr >> PAGE_SHIFT;
  int ret;

  // 计算映射表大小：静态内存页数 * 索引大小 + header 大小
  static_area_size = (static_area_size + (PAGE_SIZE - 1)) & PAGE_MASK;
  static_area_pages = static_area_size >> PAGE_SHIFT;
  map_size =
      static_area_pages * sizeof(wpmfs_vmap_entry_t) + sizeof(wpmfs_vmap_t);
  map_size = (map_size + (PAGE_SIZE - 1)) & PAGE_MASK;
  map_pages = map_size >> PAGE_SHIFT;
  psbi->vmi.map_size = map_size;

  // 最初，静态区域对应的物理内存紧随映射表之后，并线性地映射到 vmalloc space。
  // 为此，首先更新映射表，并安排好哪些页将被线性地映射到映射区
  level = 0;
  PM_EQU(pvmap->level, cpu_to_le64(level));
  PM_EQU(pvmap->num_entries, cpu_to_le64(0));

  ppages = (struct page **)kmalloc_array(static_area_pages,
                                         sizeof(struct page *), GFP_KERNEL);
  if (!ppages) goto out_nomem;

  vmap_entry0 = (wpmfs_vmap_entry_t *)(pvmap + 1);
  for (cur_page = 0; cur_page < static_area_pages; ++cur_page) {
    unsigned long cur_pfn = pfn0 + map_pages + cur_page;
    PM_EQU((vmap_entry0 + cur_page)->entry, cur_pfn);
    ppages[cur_page] = pfn_to_page(cur_pfn);
  }
  pmfs_flush_buffer(vmap_entry0, sizeof(wpmfs_vmap_entry_t) * static_area_pages,
                    false);

  // map given pages to vmalloc space through vmap
  vmaddr = vmap(ppages, static_area_pages, VM_MAP, PAGE_KERNEL);
  kfree(ppages);
  if (!vmaddr) goto out_nomem;
  psbi->vmi.addr = vmaddr;

  // 记录条目数，标志着当前事务完成
  PM_EQU(pvmap->num_entries, cpu_to_le64(static_area_pages));
  pmfs_flush_buffer(pvmap, sizeof(wpmfs_vmap_t), true);

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

int wpmfs_init(struct super_block *sb, u64 static_area_size) {
  // TODO: to replace pmfs_init
  // TODO: when not to init a new fs instance
  int errno;
  if (!_check_congfigs()) return -1;
  if (!_borrow_symbols()) return -1;

  wpmfs_init_all_cnter();
  if ((errno = _init_int(sb)) != 0) return errno;
  if ((errno = _init_mem(sb, static_area_size)) != 0) return errno;

  return 0;
}

static void _exit_int(void) {
  /* Safely destroy a workqueue. */
  if (_int_ctrl.workqueue) {
    // TODO: seems not all the works in workqueue will be executed. fix me.
    unsigned long lenq = kfifo_len(&_int_ctrl.fifo) / sizeof(unsigned long);
    if (lenq) wpmfs_error("kfifo remains stained. queue length = %lu.\n", lenq);

    destroy_workqueue(_int_ctrl.workqueue);
  }
  kfifo_free(&_int_ctrl.fifo);
}

static void _exit_mem(struct super_block *sb) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  vunmap(sbi->vmi.addr);
}

void wpmfs_exit(struct super_block *sb) {
  _exit_int();
  _exit_mem(sb);
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

  pmfs_info("vmalloc space:\n");

  wpmfs_assert(is_vmalloc_addr(super));
  pmfs_info("Superblock - start at %px, len %lu.\n", super,
            sizeof(struct pmfs_super_block));
  wpmfs_assert(is_vmalloc_addr(inode_table));
  pmfs_info("Inode table - start at %px, len %lu.\n", inode_table,
            sizeof(struct pmfs_inode));
  wpmfs_assert(is_vmalloc_addr(journal_meta));
  pmfs_info("Journal meta - start at %px, len %lu.\n", journal_meta,
            sizeof(pmfs_journal_t));
  wpmfs_assert(is_vmalloc_addr(journal_data));
  pmfs_info("Journal data - start at %px, len %d.\n", journal_data,
            cpu_to_le32(journal_meta->size));
  // TODO with vmap
  pmfs_info("direct mapping: len reserverd %lu.\n", reserved_size);

  pmfs_info("datablks - starts at %px.\n", pdatablk);
  datablk_off =
      pmfs_get_block_off(sb, sbi->unused_block_low, PMFS_BLOCK_TYPE_4K);
  pdatablk = pmfs_get_block(sb, datablk_off);
  pmfs_info("datablks - currently first free at %px, total free cnts %lu.\n",
            pdatablk, sbi->num_free_blocks);
}
