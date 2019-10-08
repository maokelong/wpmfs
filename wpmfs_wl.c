#include "wpmfs_wl.h"
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

void wpmfs_mark_page(struct page *page, enum WPMFS_PAGE_STATE op) {
  switch (op) {
    case WPMFS_PAGE_FREE:
      page->private = 0;
      break;

    case WPMFS_PAGE_USING:
      page->private = 1;
      break;

    default:
      wpmfs_assert(0);
  }
}

enum WPMFS_PAGE_STATE wpmfs_check_page(struct page *page) {
  if (page->private == 0) return WPMFS_PAGE_FREE;
  if (page->private == 1) return WPMFS_PAGE_USING;
  return WPMFS_PAGE_INVALID_STATE;
}

static bool _fetch_int_requests(unsigned long *ppfn) {
  // 这里使用最简单的请求调度算法，不对页迁移请求进行任何合并操作
  return kfifo_out_spinlocked(&_int_ctrl.fifo, ppfn, sizeof(*ppfn),
                              &_int_ctrl.fifo_lock);
}

// 设置了 rmap 的页，即为映射到进程地址空间的页
static inline bool _check_rmap(unsigned long pfn) {
  struct page *page = pfn_to_page(pfn);
  return page_mapping(page) != NULL;
}

static bool _check_vmap(unsigned long pfn) {
  // TODO:
  return true;
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
                                   void __rcu ***pslot) {
  void *entry;
  void __rcu **slot;

  xa_lock_irq(&mapping->i_pages);
  slot = radix_tree_lookup_slot(&mapping->i_pages, index);

  if (unlikely(!slot) || slot_locked(mapping, slot)) {
    xa_unlock_irq(&mapping->i_pages);
    return ERR_PTR(-EAGAIN);
  }

  entry = lock_slot(mapping, slot);
  wpmfs_assert(entry && radix_tree_exceptional_entry(entry) &&
               dax_is_pte_entry(entry));

  xa_unlock_irq(&mapping->i_pages);

  if (pslot) *pslot = slot;
  return entry;
}

static void put_locked_mapping_entry(struct address_space *mapping,
                                     pgoff_t index) {
  void *entry, **slot;

  xa_lock_irq(&mapping->i_pages);
  slot = radix_tree_lookup_slot(&mapping->i_pages, index);
  if (unlikely(!slot)) {
    xa_unlock_irq(&mapping->i_pages);
    return;
  }

  entry = radix_tree_deref_slot_protected(slot, &mapping->i_pages.xa_lock);
  if (unlikely(!radix_tree_exceptional_entry(entry)) ||
      unlikely(!slot_locked(mapping, slot))) {
    xa_unlock_irq(&mapping->i_pages);
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

static int _level_type_rmap_core(struct page *page, struct page *new_page,
                                 unsigned long blocknr) {
  struct address_space *mapping = page_mapping(page);
  struct inode *inode = mapping->host;
  pgoff_t pgoff = page_index(page);
  unsigned long new_pfn = pfn_t_to_pfn(page_to_pfn_t(new_page));
  unsigned long flags;
  void *entry, *new_entry;
  void **slot;

  wpmfs_assert(!PageTransHuge(page));
  entry = trygrab_mapping_entry(mapping, pgoff, &slot);
  if (IS_ERR(entry)) return -EAGAIN;

  new_page->lru = page->lru;
  new_page->index = page->index;
  new_page->mapping = page->mapping;
  flags = (unsigned long)entry & ((1 << RADIX_DAX_SHIFT) - 1);
  new_entry = dax_radix_locked_entry(new_pfn, flags);

  unmap_mapping_range(mapping, pgoff << PAGE_SHIFT, PAGE_SIZE, false);
  page->mapping = NULL;
  page->index = 0;

  wpmfs_replace_single_datablk(inode, pgoff, blocknr);

  radix_tree_replace_slot(&mapping->i_pages, slot, new_entry);

  put_locked_mapping_entry(mapping, pgoff);

  return MIGRATEPAGE_SUCCESS;
}

static void _level_type_rmap(struct super_block *sb, unsigned long pfn) {
  unsigned long blocknr;
  u64 blockoff;
  struct page *page = pfn_to_page(pfn);
  struct page *new_page = NULL;
  struct inode *inode = page->mapping->host;
  int errval;

  if (!inode) {
    wpmfs_error("Fatal. Inode is missing.\n");
    return;
  }

  errval = pmfs_new_block(sb, &blocknr, PMFS_BLOCK_TYPE_4K, false);
  if (errval == -ENOMEM) {
    wpmfs_error("Migration(Case Rmap) failed. Memory exhausted.\n");
    return;
  }

  blockoff = pmfs_get_block_off(sb, blocknr, PMFS_BLOCK_TYPE_4K);
  new_page = pfn_to_page(pmfs_get_pfn(sb, blockoff));

  // 加读锁，因为页迁移过程是安全的，而且不影响读者所见结果
  inode_lock_shared(inode);

  errval = _level_type_rmap_core(page, new_page, blocknr);
  if (errval == -EAGAIN) {
    wpmfs_dbg_wl_rmap("Migration(Case Rmap) failed. pfn = %lu.\n", pfn);
    inode_unlock_shared(inode);

    pmfs_free_block(sb, blocknr, PMFS_BLOCK_TYPE_4K);
    return;
  } else {
    blocknr = wpmfs_get_blocknr(sb, pfn);
    pmfs_free_block(sb, blocknr, PMFS_BLOCK_TYPE_4K);
  }

  inode_unlock_shared(inode);
  wpmfs_dbg_wl_rmap("Migration(Case Rmap) succeed. pgoff = %lu.\n",
                    new_page->index);
  // TODO: 检查 inode，看是否需要对元数据进行迁移。
}

static void _level_type_vmap(struct super_block *sb, unsigned long pfn) {
  // TODO
  wpmfs_error("impossible routine currently.\n");
}

static void _level_type_stranded(struct super_block *sb, unsigned long pfn) {
  // TODO:
  wpmfs_error("impossible routine currently.\n");
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

  if (wpmfs_check_page(pfn_to_page(pfn)) == WPMFS_PAGE_USING)
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
  if (!_check_congfigs()) return -1;
  if (!_borrow_symbols()) return -1;

  wpmfs_init_all_cnter();
  _init_int(sb);
  _init_mem(sb, static_area_size);
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
