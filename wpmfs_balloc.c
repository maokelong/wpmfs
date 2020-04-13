#include <linux/bitops.h>
#include <linux/fs.h>
#include "pmfs.h"
#include "wpmfs_wl.h"

/*************************************************
 * SN: 0
 * simples allocator that allocates pages suffered minimal writes
 *************************************************/

static int wpmfs_get_bin(struct super_block* sb, unsigned long blocknr) {
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	u64 blockoff = wpmfs_get_blockoff(sb, blocknr, 0).blockoff;
	u64 pfn = pmfs_get_pfn(sb, blockoff);
	int target_bin = (int)(wt_cnter_read_pfn(pfn) / get_int_thres_size());
	return (target_bin < sbi->num_bins) ? target_bin : sbi->num_bins - 1;
}

void wpmfs_init_blockmap(struct super_block *sb, unsigned long init_used_size) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  unsigned long num_used_block;

  /* record all unused pages  */
  wpmfs_assert(sb->s_blocksize == PAGE_SIZE);
  num_used_block =
      (init_used_size + sb->s_blocksize - 1) >> sb->s_blocksize_bits;
  sbi->unused_block_low = sbi->block_start + num_used_block;
  sbi->unused_block_high = sbi->block_end;

  sbi->num_free_blocks -= num_used_block;
}

void __wpmfs_free_block(struct super_block *sb, unsigned long blocknr,
                        unsigned short btype, void **private) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  unsigned long num_blocks = 0;
  int target_bin;
  struct list_head *new_node;
  u64 blockoff = pmfs_get_block_off(sb, blocknr, PMFS_BLOCK_TYPE_4K);
  struct page *page;
  struct list_head *pl;

  /* huge block is not supported */
  num_blocks = pmfs_get_numblocks(btype);
  wpmfs_assert(num_blocks == 1);
  /* private(start_hint) should never been used here */
  wpmfs_assert(private == NULL || *(struct pmfs_blocknode **)private == NULL);

  /* insert the block into appropriate bin */
  target_bin = wpmfs_get_bin(sb, blocknr);
  new_node = (struct list_head *)pmfs_get_block(sb, blockoff);
  pl = &sbi->block_bins[target_bin];
  PM_TOUCH(new_node, sizeof(*new_node));
  if (!list_empty(pl)) PM_TOUCH(&pl->next->prev, sizeof(pl->next->prev));
  list_add(new_node, pl);

  /* update statistic info */
  sbi->num_free_blocks += num_blocks;

  page = pfn_to_page(pmfs_get_pfn(sb, blockoff));
  wpmfs_mark_page(page, wpmfs_page_marks(page), WPMFS_PAGE_USING);
}

void wpmfs_free_block(struct super_block *sb, unsigned long blocknr,
                      unsigned short btype) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);

  mutex_lock(&sbi->s_lock);
  __wpmfs_free_block(sb, blocknr, btype, NULL);
  mutex_unlock(&sbi->s_lock);
}

int __wpmfs_new_block(struct super_block *sb, unsigned long *blocknr,
                      unsigned short btype, int zero) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  int cur_bin, num_bins = sbi->num_bins;
  int errval = 0;
  unsigned long num_blocks = 0, num_unused_blocks;
  u64 blockoff;
  struct page *page;

  /* huge block is not supported */
  num_blocks = pmfs_get_numblocks(btype);
  wpmfs_assert(num_blocks == 1);

  /* try to allocate unused blocks first to "active" these blocks */
  num_unused_blocks = sbi->unused_block_high - sbi->unused_block_low;
  if (num_unused_blocks) {
    *blocknr = sbi->unused_block_low++;
    goto new_suc;
  }

  /* search bins to retrieve the least worn free block */
  for (cur_bin = 0; sbi->num_free_blocks && cur_bin < num_bins - 1; ++cur_bin) {
    struct list_head *lcur = &sbi->block_bins[cur_bin], *entry;
    if (list_empty(lcur)) continue;
    entry = lcur->prev;
    list_del(entry);
    if (!list_empty(lcur))
      PM_TOUCH(&lcur->prev->next, sizeof(lcur->prev->next));

    *blocknr = wpmfs_reget_blocknr(sb, wpmfs_reget_blockoff(sb, entry));
    goto new_suc;
  }

  /* no free block */
  errval = -ENOMEM;
  goto new_fail;

new_suc:
  if (zero) {
    void *block = pmfs_get_block(sb, pmfs_get_block_off(sb, *blocknr, 0));
    memset_nt(block, 0, 0x1 << 12);
  }

  sbi->num_free_blocks--;

  blockoff = pmfs_get_block_off(sb, *blocknr, PMFS_BLOCK_TYPE_4K);
  page = pfn_to_page(pmfs_get_pfn(sb, blockoff));
  wpmfs_mark_page(page, wpmfs_page_marks(page), WPMFS_PAGE_USING);

new_fail:
  return errval;
}

int wpmfs_new_block(struct super_block *sb, unsigned long *blocknr,
                    unsigned short btype, int zero) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  int errval = 0;
  mutex_lock(&sbi->s_lock);
  errval = __wpmfs_new_block(sb, blocknr, btype, zero);
  mutex_unlock(&sbi->s_lock);
  return errval;
}

unsigned long wpmfs_count_free_blocks(struct super_block *sb) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  return sbi->num_free_blocks;
}

/*************************************************
 * SN: 1
 * PMFS's default allocator
 *************************************************/

void pmfs_init_blockmap(struct super_block *sb, unsigned long init_used_size) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  unsigned long num_used_block;
  struct pmfs_blocknode *blknode;

  num_used_block =
      (init_used_size + sb->s_blocksize - 1) >> sb->s_blocksize_bits;

  blknode = pmfs_alloc_blocknode(sb);
  if (blknode == NULL) PMFS_ASSERT(0);
  blknode->block_low = sbi->block_start;
  blknode->block_high = sbi->block_start + num_used_block - 1;
  sbi->num_free_blocks -= num_used_block;
  list_add(&blknode->link, &sbi->block_inuse_head);
}

static struct pmfs_blocknode *pmfs_next_blocknode(struct pmfs_blocknode *i,
                                                  struct list_head *head) {
  if (list_is_last(&i->link, head)) return NULL;
  return list_first_entry(&i->link, typeof(*i), link);
}

/* Caller must hold the super_block lock.  If start_hint is provided, it is
 * only valid until the caller releases the super_block lock. */
void __pmfs_free_block(struct super_block *sb, unsigned long blocknr,
                       unsigned short btype, void **private) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  struct list_head *head = &(sbi->block_inuse_head);
  unsigned long new_block_low;
  unsigned long new_block_high;
  unsigned long num_blocks = 0;
  struct pmfs_blocknode *i;
  struct pmfs_blocknode *free_blocknode = NULL;
  struct pmfs_blocknode *curr_node;
  struct pmfs_blocknode **start_hint = (struct pmfs_blocknode **)private;

  num_blocks = pmfs_get_numblocks(btype);
  new_block_low = blocknr;
  new_block_high = blocknr + num_blocks - 1;

  BUG_ON(list_empty(head));

  if (start_hint && *start_hint && new_block_low >= (*start_hint)->block_low)
    i = *start_hint;
  else
    i = list_first_entry(head, typeof(*i), link);

  list_for_each_entry_from(i, head, link) {
    if (new_block_low > i->block_high) {
      /* skip to next blocknode */
      continue;
    }

    if ((new_block_low == i->block_low) && (new_block_high == i->block_high)) {
      /* fits entire datablock */
      if (start_hint) *start_hint = pmfs_next_blocknode(i, head);
      list_del(&i->link);
      free_blocknode = i;
      sbi->num_blocknode_allocated--;
      sbi->num_free_blocks += num_blocks;
      goto block_found;
    }
    if ((new_block_low == i->block_low) && (new_block_high < i->block_high)) {
      /* Aligns left */
      i->block_low = new_block_high + 1;
      sbi->num_free_blocks += num_blocks;
      if (start_hint) *start_hint = i;
      goto block_found;
    }
    if ((new_block_low > i->block_low) && (new_block_high == i->block_high)) {
      /* Aligns right */
      i->block_high = new_block_low - 1;
      sbi->num_free_blocks += num_blocks;
      if (start_hint) *start_hint = pmfs_next_blocknode(i, head);
      goto block_found;
    }
    if ((new_block_low > i->block_low) && (new_block_high < i->block_high)) {
      /* Aligns somewhere in the middle */
      curr_node = pmfs_alloc_blocknode(sb);
      PMFS_ASSERT(curr_node);
      if (curr_node == NULL) {
        /* returning without freeing the block*/
        goto block_found;
      }
      curr_node->block_low = new_block_high + 1;
      curr_node->block_high = i->block_high;
      i->block_high = new_block_low - 1;
      list_add(&curr_node->link, &i->link);
      sbi->num_free_blocks += num_blocks;
      if (start_hint) *start_hint = curr_node;
      goto block_found;
    }
  }

  pmfs_error_mng(sb, "Unable to free block %ld\n", blocknr);

block_found:

  if (free_blocknode) __pmfs_free_blocknode(free_blocknode);
}

void pmfs_free_block(struct super_block *sb, unsigned long blocknr,
                     unsigned short btype) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  mutex_lock(&sbi->s_lock);
  __pmfs_free_block(sb, blocknr, btype, NULL);
  mutex_unlock(&sbi->s_lock);
}

int pmfs_new_block(struct super_block *sb, unsigned long *blocknr,
                   unsigned short btype, int zero) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  struct list_head *head = &(sbi->block_inuse_head);
  struct pmfs_blocknode *i, *next_i;
  struct pmfs_blocknode *free_blocknode = NULL;
  void *bp;
  unsigned long num_blocks = 0;
  struct pmfs_blocknode *curr_node;
  int errval = 0;
  bool found = 0;
  unsigned long next_block_low;
  unsigned long new_block_low;
  unsigned long new_block_high;

  num_blocks = pmfs_get_numblocks(btype);

  mutex_lock(&sbi->s_lock);

  list_for_each_entry(i, head, link) {
    if (i->link.next == head) {
      next_i = NULL;
      next_block_low = sbi->block_end;
    } else {
      next_i = list_entry(i->link.next, typeof(*i), link);
      next_block_low = next_i->block_low;
    }

    new_block_low = (i->block_high + num_blocks) & ~(num_blocks - 1);
    new_block_high = new_block_low + num_blocks - 1;

    if (new_block_high >= next_block_low) {
      /* Does not fit - skip to next blocknode */
      continue;
    }

    if ((new_block_low == (i->block_high + 1)) &&
        (new_block_high == (next_block_low - 1))) {
      /* Fill the gap completely */
      if (next_i) {
        i->block_high = next_i->block_high;
        list_del(&next_i->link);
        free_blocknode = next_i;
        sbi->num_blocknode_allocated--;
      } else {
        i->block_high = new_block_high;
      }
      found = 1;
      break;
    }

    if ((new_block_low == (i->block_high + 1)) &&
        (new_block_high < (next_block_low - 1))) {
      /* Aligns to left */
      i->block_high = new_block_high;
      found = 1;
      break;
    }

    if ((new_block_low > (i->block_high + 1)) &&
        (new_block_high == (next_block_low - 1))) {
      /* Aligns to right */
      if (next_i) {
        /* right node exist */
        next_i->block_low = new_block_low;
      } else {
        /* right node does NOT exist */
        curr_node = pmfs_alloc_blocknode(sb);
        PMFS_ASSERT(curr_node);
        if (curr_node == NULL) {
          errval = -ENOSPC;
          break;
        }
        curr_node->block_low = new_block_low;
        curr_node->block_high = new_block_high;
        list_add(&curr_node->link, &i->link);
      }
      found = 1;
      break;
    }

    if ((new_block_low > (i->block_high + 1)) &&
        (new_block_high < (next_block_low - 1))) {
      /* Aligns somewhere in the middle */
      curr_node = pmfs_alloc_blocknode(sb);
      PMFS_ASSERT(curr_node);
      if (curr_node == NULL) {
        errval = -ENOSPC;
        break;
      }
      curr_node->block_low = new_block_low;
      curr_node->block_high = new_block_high;
      list_add(&curr_node->link, &i->link);
      found = 1;
      break;
    }
  }

  if (found == 1) {
    sbi->num_free_blocks -= num_blocks;
  }

  mutex_unlock(&sbi->s_lock);

  if (free_blocknode) __pmfs_free_blocknode(free_blocknode);

  if (found == 0) {
    return -ENOSPC;
  }

  if (zero) {
    size_t size;
    bp = pmfs_get_block(sb, pmfs_get_block_off(sb, new_block_low, btype));
    pmfs_memunlock_block(sb, bp);  // TBDTBD: Need to fix this
    if (btype == PMFS_BLOCK_TYPE_4K)
      size = 0x1 << 12;
    else if (btype == PMFS_BLOCK_TYPE_2M)
      size = 0x1 << 21;
    else
      size = 0x1 << 30;
    memset_nt(bp, 0, size);
    pmfs_memlock_block(sb, bp);
  }
  *blocknr = new_block_low;

  return errval;
}

unsigned long pmfs_count_free_blocks(struct super_block *sb) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  return sbi->num_free_blocks;
}

/*************************************************
 * Allocator Factory
 *************************************************/

struct allocator_factory Allocator = {.pmfs_new_block = NULL,
                                      .pmfs_free_block = NULL,
                                      .__pmfs_free_block = NULL,
                                      .pmfs_init_blockmap = NULL,
                                      .pmfs_setup_blocknode_map = NULL};

bool wpmfs_select_allocator(int alloc) {
  switch (alloc) {
    case 0:
      Allocator.pmfs_count_free_blocks = wpmfs_count_free_blocks;
      Allocator.pmfs_new_block = wpmfs_new_block;
      Allocator.pmfs_free_block = wpmfs_free_block;
      Allocator.__pmfs_free_block = __wpmfs_free_block;
      Allocator.pmfs_init_blockmap = wpmfs_init_blockmap;
      Allocator.pmfs_setup_blocknode_map = wpmfs_setup_blocknode_map;
      break;

    case 1:
      Allocator.pmfs_count_free_blocks = pmfs_count_free_blocks;
      Allocator.pmfs_new_block = pmfs_new_block;
      Allocator.pmfs_free_block = pmfs_free_block;
      Allocator.__pmfs_free_block = __pmfs_free_block;
      Allocator.pmfs_init_blockmap = pmfs_init_blockmap;
      Allocator.pmfs_setup_blocknode_map = pmfs_setup_blocknode_map;
      break;

    default:
      wpmfs_error("Invalid allocator.\n");
      return false;
  }

  return true;
}

/*************************************************
 * Additional memory management functions
 * introduced by wpmfs
 *************************************************/

unsigned long wpmfs_get_pfn(struct super_block *sb, u64 blockoff) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  unsigned long pfn0 = sbi->phys_addr >> PAGE_SHIFT;
  wb *_wb = (wb *)(&blockoff);

  switch (_wb->vlocation) {
    case 0:
      return pfn0 + (blockoff >> PAGE_SHIFT);

    case 2: {
      mptable_slot_t *mptable = wpmfs_get_mptable_dynamic(sb);
      return pfn0 + le64_to_cpu(mptable[_wb->val >> PAGE_SHIFT].blocknr);
    }

    default:
      wpmfs_assert(0);
      return 0;
  }
}

unsigned long wpmfs_reget_blocknr(struct super_block *sb, u64 blockoff) {
  wb *_wb = (wb *)(&blockoff);

  switch (_wb->vlocation) {
    case 0:
      return blockoff >> PAGE_SHIFT;

    case 2: {
      mptable_slot_t *mptable = wpmfs_get_mptable_dynamic(sb);
      return le64_to_cpu(mptable[_wb->val >> PAGE_SHIFT].blocknr);
    }

    default:
      wpmfs_assert(0);
      return 0;
  }
}

// TODO: 考虑如何 unmap
// TODO：放进事务里面
// 因为 inode 及 log 中均存储 blockoff，因此这里也返回 blockoff
wb wpmfs_map_dynamic_page(struct super_block *sb, u64 blocknr) {
  extern int vmap_page_range(unsigned long start, unsigned long end,
                             pgprot_t prot, struct page **pages);

  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  mptable_slot_t *mptable = wpmfs_get_mptable_dynamic(sb);
  wb blockoff;
  u64 size = sbi->vmapi.size_dynamic, index = size >> PAGE_SHIFT;
  unsigned long start_addr = (unsigned long)(sbi->vmapi.base_dynamic + size);
  struct page *page;

  blockoff = wpmfs_get_blockoff(sb, blocknr, 0);
  page = pfn_to_page(wpmfs_get_pfn(sb, blockoff.blockoff));
  vmap_page_range(start_addr, start_addr + PAGE_SIZE, PAGE_KERNEL, &page);
  wpmfs_mark_page(page, wpmfs_page_marks(page),
                  WPMFS_PAGE_USING | WPMFS_PAGE_VMAP | WPMFS_PAGE_VMAP_DYNAMIC);
  page->index = index;
  blockoff.val = size;
  blockoff.vlocation = 2;

  PM_EQU(mptable[index].blocknr, cpu_to_le64(blocknr));
  pmfs_flush_buffer(mptable + size, sizeof(*mptable), true);

  sbi->vmapi.size_dynamic += PAGE_SIZE;

  return blockoff;
}

u64 wpmfs_reget_blockoff(struct super_block *sb, void *addr) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  wb blockoff = {0};

  if (!is_vmalloc_addr(addr)) {
    blockoff.val = (u64)(addr - sbi->virt_addr);
    blockoff.vlocation = 0;
    return blockoff.blockoff;
  }

  if (addr >= sbi->vmapi.base_static &&
      addr < (sbi->vmapi.base_static + sbi->vmapi.size_static)) {
    blockoff.val = (u64)(addr - sbi->vmapi.base_static);
    blockoff.vlocation = 1;
    return blockoff.blockoff;
  }

  if (addr >= sbi->vmapi.base_dynamic &&
      addr < (sbi->vmapi.base_dynamic + sbi->vmapi.size_dynamic)) {
    blockoff.val = (u64)(addr - sbi->vmapi.base_dynamic);
    blockoff.vlocation = 2;
    return blockoff.blockoff;
  }

  wpmfs_assert(0);
  return blockoff.blockoff;
}

void *wpmfs_reget_mptable_slot(struct super_block *sb, struct page *page) {
  bool vmap_dynamic = wpmfs_page_marks(page) & WPMFS_PAGE_VMAP_DYNAMIC;

  if (!vmap_dynamic)
    return wpmfs_get_mptable_static(sb) + page->index;
  else
    return wpmfs_get_mptable_dynamic(sb) + page->index;
}

/*************************************************
 * Reserve Blocks for WPMFS
 *************************************************/

static bool setup_vmap_static(struct super_block *sb,
                              u64 *reserved_memory_size) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  struct wpmfs_mptables *mptables = wpmfs_get_mptables(sb);
  mptable_slot_t *mptable = wpmfs_get_mptable_static(sb);
  u64 rsrv_mem_size, rsrv_mem_pages;
  u64 map_size, map_pages;
  struct page **ppages = NULL;
  void *vmaddr;
  u64 cur_slot;
  unsigned long pfn0 = sbi->phys_addr >> PAGE_SHIFT;
  bool succ = false;

  PM_EQU(mptables->mptable_static.num_pages, 0);

  rsrv_mem_size = *reserved_memory_size;
  rsrv_mem_size = (rsrv_mem_size + (PAGE_SIZE - 1)) & PAGE_MASK;
  rsrv_mem_pages = rsrv_mem_size >> PAGE_SHIFT;

  map_size = sizeof(struct wpmfs_mptables);
  map_size += rsrv_mem_pages * sizeof(mptable_slot_t);
  map_size = (map_size + (PAGE_SIZE - 1)) & PAGE_MASK;
  map_pages = map_size >> PAGE_SHIFT;

  ppages = (struct page **)vmalloc(rsrv_mem_pages * sizeof(struct page *));
  if (!ppages) goto out_nomem;

  for (cur_slot = 0; cur_slot < rsrv_mem_pages; ++cur_slot) {
    u64 blocknr = map_pages + cur_slot;
    struct page *page = pfn_to_page(pfn0 + blocknr);
    PM_EQU(mptable[cur_slot].blocknr, cpu_to_le64(blocknr));
    ppages[cur_slot] = page;

    wpmfs_mark_page(page, wpmfs_page_marks(page),
                    WPMFS_PAGE_USING | WPMFS_PAGE_VMAP);
    page->index = cur_slot;
  }

  vmaddr = vmap(ppages, rsrv_mem_pages, VM_MAP, PAGE_KERNEL);
  vfree(ppages);
  ppages = NULL;
  if (!vmaddr) goto out_nomem;

  sbi->vmapi.base_static = vmaddr;
  sbi->vmapi.size_static = rsrv_mem_size;

  pmfs_flush_buffer(mptables, map_size, true);
  PM_EQU(mptables->mptable_static.num_pages, cpu_to_le64(rsrv_mem_pages));
  pmfs_flush_buffer(mptables, sizeof(struct wpmfs_mptables), true);

  *reserved_memory_size += map_size;
  succ = true;

out_nomem:
  if (ppages) vfree(ppages);
  return succ;
}

static bool setup_vmap_dynamic(struct super_block *sb,
                               u64 *reserved_memory_size) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  struct wpmfs_mptables *mptables = wpmfs_get_mptables(sb);
  struct vm_struct *area;
  u64 cur_page;
  u64 map_size = PAGE_SIZE * 10, map_pages;
  unsigned long pfn0 = sbi->phys_addr >> PAGE_SHIFT;
  const u64 vm_size = 1024 * 1024 * 1024;
  bool succ = false;

  map_size = (map_size + (PAGE_SIZE - 1)) & PAGE_MASK;
  map_pages = map_size >> PAGE_SHIFT;

  for (cur_page = 0; cur_page < map_pages; ++cur_page) {
    u64 blocknr = ((*reserved_memory_size) >> PAGE_SHIFT) + cur_page;
    struct page *page = pfn_to_page(pfn0 + blocknr);
    void *block = wpmfs_get_block(sb, pmfs_get_block_off(sb, blocknr, 0));

    PM_MEMSET(block, 0, PAGE_SIZE);
    pmfs_flush_buffer(block, PAGE_SIZE, false);
    wpmfs_mark_page(page, wpmfs_page_marks(page),
                    WPMFS_PAGE_USING | WPMFS_PAGE_VMAP_DYNAMIC);
  }

  mptables->mptable_dynamic.head_blocknr =
      cpu_to_le64((*reserved_memory_size) >> PAGE_SHIFT);
  mptables->mptable_dynamic.num_pages = cpu_to_le64(0);
  pmfs_flush_buffer(mptables, sizeof(struct wpmfs_mptables), true);

  area = get_vm_area_caller(vm_size, VM_MAP, __builtin_return_address(0));
  if (!area) goto out_nomem;
  sbi->vmapi.base_dynamic = area->addr;
  sbi->vmapi.size_dynamic = 0;

  *reserved_memory_size += map_size;
  succ = true;

out_nomem:
  return succ;
}

int wpmfs_setup_memory(struct super_block *sb, u64 *reserved_memory_size) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  bool succ = false;
  INIT_TIMING(setup_vmap_time);

  if (!sbi->vmapi.enabled) goto out_succ;

  PMFS_START_TIMING(setup_vmap_t, setup_vmap_time);

  if (!setup_vmap_static(sb, reserved_memory_size)) goto out_fail;
  if (!setup_vmap_dynamic(sb, reserved_memory_size)) goto out_fail;

out_succ:
  succ = true;

out_fail:
  PMFS_END_TIMING(setup_vmap_t, setup_vmap_time);
  return succ;
}
