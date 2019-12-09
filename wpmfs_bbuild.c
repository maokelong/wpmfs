/*
 * PMFS emulated persistence. This file contains code to 
 * handle data blocks of various sizes efficiently.
 *
 * Persistent Memory File System
 * Copyright (c) 2012-2013, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <linux/fs.h>
#include <linux/bitops.h>
#include <linux/slab.h>
#include "pmfs.h"

struct scan_bitmap {
	unsigned long bitmap_4k_size;
	unsigned long *bitmap_4k;
};

static void wpmfs_inode_crawl_recursive(struct super_block *sb,
				struct scan_bitmap *bm, unsigned long block,
				u32 height, u8 btype)
{
	__le64 *node;
	unsigned int i;

	if (height == 0) {
		/* This is the data block */
		wpmfs_assert(btype == PMFS_BLOCK_TYPE_4K);
		set_bit(block >> PAGE_SHIFT, bm->bitmap_4k);
		return;
	}

	node = pmfs_get_block(sb, block);
	set_bit(block >> PAGE_SHIFT, bm->bitmap_4k);
	for (i = 0; i < (1 << META_BLK_SHIFT); i++) {
		if (node[i] == 0)
			continue;
		wpmfs_inode_crawl_recursive(sb, bm,
			le64_to_cpu(node[i]), height - 1, btype);
	}
}

static inline void wpmfs_inode_crawl(struct super_block *sb,
				struct scan_bitmap *bm, struct pmfs_inode *pi)
{
	if (pi->root == 0)
		return;
	wpmfs_inode_crawl_recursive(sb, bm, le64_to_cpu(pi->root), pi->height,
					pi->i_blk_type);
}

static void wpmfs_inode_table_crawl_recursive(struct super_block *sb,
				struct scan_bitmap *bm, unsigned long block,
				u32 height, u32 btype)
{
	__le64 *node;
	unsigned int i;
	struct pmfs_inode *pi;
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	
	node = pmfs_get_block(sb, block);

	if (height == 0) {
		unsigned int inodes_per_block = INODES_PER_BLOCK(btype);
		wpmfs_assert(btype == PMFS_BLOCK_TYPE_4K);
		set_bit(block >> PAGE_SHIFT, bm->bitmap_4k);

		sbi->s_inodes_count += inodes_per_block;
		for (i = 0; i < inodes_per_block; i++) {
			pi = (struct pmfs_inode *)((void *)node +
                                                        PMFS_INODE_SIZE * i);
			if (le16_to_cpu(pi->i_links_count) == 0 &&
                        	(le16_to_cpu(pi->i_mode) == 0 ||
                         	le32_to_cpu(pi->i_dtime))) {
					/* Empty inode */
					continue;
			}
			sbi->s_inodes_used_count++;
			wpmfs_inode_crawl(sb, bm, pi);
		}
		return;
	}

	set_bit(block >> PAGE_SHIFT, bm->bitmap_4k);
	for (i = 0; i < (1 << META_BLK_SHIFT); i++) {
		if (node[i] == 0)
			continue;
		wpmfs_inode_table_crawl_recursive(sb, bm,
			le64_to_cpu(node[i]), height - 1, btype);
	}
}

static void wpmfs_try_free_blocks(struct super_block *sb, unsigned long low,
                                  unsigned long high) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  unsigned long pfn0 = sbi->phys_addr >> PAGE_SHIFT;

  for (; low < high; ++low)
    if (!(wpmfs_page_marks(pfn_to_page(low + pfn0)) & WPMFS_PAGE_USING)) {
      __wpmfs_free_block(sb, low, PMFS_BLOCK_TYPE_4K, NULL);
			sbi->num_free_blocks++;
		}
}

static int __wpmfs_build_blocknode_map(struct super_block *sb,
                                      unsigned long *bitmap,
                                      unsigned long bsize,
                                      unsigned long scale) {
  unsigned long next = 1;
  unsigned long low = 0;

  while (1) {
    next = find_next_zero_bit(bitmap, bsize, next);
    if (next == bsize) break;
    low = next;
    next = find_next_bit(bitmap, bsize, next);
    wpmfs_try_free_blocks(sb, low, next);
    if (next == bsize) break;
  }

  if (low == 0) wpmfs_try_free_blocks(sb, low, next);

  return 0;
}

static void wpmfs_build_blocknode_map(struct super_block *sb,
							struct scan_bitmap *bm)
{
	__wpmfs_build_blocknode_map(sb, bm->bitmap_4k, bm->bitmap_4k_size * 8,
		PAGE_SHIFT - 12);
}

int wpmfs_setup_blocknode_map(struct super_block *sb)
{
	struct pmfs_super_block *super = pmfs_get_super(sb);
	struct pmfs_inode *pi = pmfs_get_inode_table(sb);
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	struct scan_bitmap bm;
	unsigned long initsize = le64_to_cpu(super->s_size);
	timing_t start, end;

	/* Always check recovery time */
	if (measure_timing == 0)
		getrawmonotonic(&start);

	PMFS_START_TIMING(recovery_t, start);

	mutex_init(&sbi->inode_table_mutex);
	sbi->block_start = (unsigned long)0;
	sbi->block_end = ((unsigned long)(initsize) >> PAGE_SHIFT);
	
	pmfs_dbg("PMFS: Performing failure recovery\n");
	bm.bitmap_4k_size = (initsize >> (PAGE_SHIFT + 0x3)) + 1;

	/* Alloc memory to hold the block alloc bitmap */
	bm.bitmap_4k = kzalloc(bm.bitmap_4k_size, GFP_KERNEL);

	if (!bm.bitmap_4k)
		goto skip;
	
	wpmfs_inode_table_crawl_recursive(sb, &bm, le64_to_cpu(pi->root),
						pi->height, pi->i_blk_type);

	/* Reserving tow inodes - Inode 0 and Inode for datablock */
	sbi->s_free_inodes_count = sbi->s_inodes_count -  
		(sbi->s_inodes_used_count + 2);
	
	/* set the block 0 as this is used */
	sbi->s_free_inode_hint = PMFS_FREE_INODE_HINT_START;

	/* initialize the num_free_blocks to */
	sbi->num_free_blocks = ((unsigned long)(initsize) >> PAGE_SHIFT);

	// Set init_used_size to initsize.
	// wpmfs treat all blocks as allocated at first.
	wpmfs_init_blockmap(sb, initsize);
	wpmfs_build_blocknode_map(sb, &bm);

skip:
	
	kfree(bm.bitmap_4k);
	PMFS_END_TIMING(recovery_t, start);
	if (measure_timing == 0) {
		getrawmonotonic(&end);
		Timingstats[recovery_t] +=
			(end.tv_sec - start.tv_sec) * 1000000000 +
			(end.tv_nsec - start.tv_nsec);
	}

	return 0;
}
