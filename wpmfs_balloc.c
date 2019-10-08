#include <linux/bitops.h>
#include <linux/fs.h>
#include "pmfs.h"
#include "wpmfs_wl.h"

void pmfs_init_blockmap(struct super_block *sb, unsigned long init_used_size) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  int cur_bin;
  unsigned long num_used_block;

  /* allocate page bins, the last bin helds worn out pages */
  sbi->num_bins = get_cell_idea_endurance() * PAGE_SIZE / get_int_thres_size() + 1;
  sbi->block_bins = (struct list_head *)kmalloc_array(
      sbi->num_bins, sizeof(struct list_head), GFP_KERNEL);
  wpmfs_assert(sbi->block_bins);
  for (cur_bin = 0; cur_bin < sbi->num_bins; ++cur_bin) {
    INIT_LIST_HEAD(&sbi->block_bins[cur_bin]);
  }

  /* record all unused pages  */
  wpmfs_assert(sb->s_blocksize == PAGE_SIZE);
  num_used_block =
      (init_used_size + sb->s_blocksize - 1) >> sb->s_blocksize_bits;
  sbi->unused_block_low = sbi->block_start + num_used_block;
  sbi->unused_block_high = sbi->block_end;

  sbi->num_free_blocks -= num_used_block;
}

void __pmfs_free_block(struct super_block *sb, unsigned long blocknr,
                       unsigned short btype,
                       struct pmfs_blocknode **start_hint) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  unsigned long num_blocks = 0;
  int target_bin;
  struct list_head *new_node;
  u64 blockoff;
  struct page *page;

  /* huge block is not supported */
  num_blocks = pmfs_get_numblocks(btype);
  wpmfs_assert(num_blocks == 1);
  /* start_hint should never been used here */
  wpmfs_assert(start_hint == NULL);

  /* insert the block into appropriate bin */
  target_bin = wpmfs_get_bin(sb, blocknr);
  new_node = (struct list_head *)pmfs_get_block(sb, blocknr);
  list_add(new_node, &sbi->block_bins[target_bin]);

  /* update statistic info */
  sbi->num_free_blocks += num_blocks;

  blockoff = pmfs_get_block_off(sb, blocknr, PMFS_BLOCK_TYPE_4K);
  page = pfn_to_page(pmfs_get_pfn(sb, blockoff));
  wpmfs_mark_page(page, WPMFS_PAGE_FREE);
}

void pmfs_free_block(struct super_block *sb, unsigned long blocknr,
                     unsigned short btype) {  
  struct pmfs_sb_info *sbi = PMFS_SB(sb);

  mutex_lock(&sbi->s_lock);
  __pmfs_free_block(sb, blocknr, btype, NULL);
  mutex_unlock(&sbi->s_lock);
}

int _pmfs_new_block(struct super_block *sb, unsigned long *blocknr,
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
    entry = lcur->next;
    list_del(lcur->next);
    *blocknr = pmfs_get_blocknr(sb, pmfs_get_addr_off(sbi, lcur), 0);
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
  wpmfs_mark_page(page, WPMFS_PAGE_USING);

new_fail:
  return errval;
}

int pmfs_new_block(struct super_block *sb, unsigned long *blocknr,
                   unsigned short btype, int zero) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  int errval = 0;
  mutex_lock(&sbi->s_lock);
  errval = _pmfs_new_block(sb, blocknr, btype, zero);
  mutex_unlock(&sbi->s_lock);
  return errval;
}

unsigned long pmfs_count_free_blocks(struct super_block *sb) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  return sbi->num_free_blocks;
}
