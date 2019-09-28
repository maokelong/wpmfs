#ifndef WPMFS_WL_H
#define WPMFS_WL_H

#include <linux/fs.h>
#include "pmfs.h"

typedef struct wpmfs_vmap {
  __le64 level;
  __le64 num_entries;
} wpmfs_vmap_t;

typedef struct wpmfs_vmap_entry {
  u64 entry;
} wpmfs_vmap_entry_t;

static inline wpmfs_vmap_t* wpmfs_get_vmap(struct super_block* sb, int level) {
  // TODO: 当前仅实现了一级映射表
  struct pmfs_sb_info* sbi = PMFS_SB(sb);
  wpmfs_assert(level == 0);
  return (wpmfs_vmap_t*)sbi->virt_addr;
}

extern void fs_now_ready(struct block_device *fs_bdev);
extern int wpmfs_init(struct super_block* sb, u64 static_area_size);
extern void wpmfs_exit(struct super_block* sb);

#endif /* WPMFS_WL_H */
