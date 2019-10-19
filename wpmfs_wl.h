#ifndef WPMFS_WL_H
#define WPMFS_WL_H

#include <linux/fs.h>
#include "pmfs.h"

/*************************************************
 * per page vector stroing some flags (page->private)
 *************************************************/

#define WPMFS_PAGE_SHIFT (2)
#define WPMFS_PAGE_USING (1 << 0)
#define WPMFS_PAGE_TIRED (1 << 1)

static inline unsigned long wpmfs_page_marks(struct page* page) {
  atomic64_t* pmarks = (atomic64_t*)&page->private;
  return atomic_long_read(pmarks);
}

static inline bool wpmfs_mark_page(struct page* page, unsigned long ori_flags,
                                   unsigned long new_flags) {
  //  kernel bugs will report the empry page.
  atomic64_t* pmarks = (atomic64_t*)&page->private;
  if (atomic_long_cmpxchg(pmarks, ori_flags, new_flags) != ori_flags) {
    wpmfs_error("contention on page marks detected.\n");
    return false;
  }

  return true;
}

/*************************************************
 * descriptor for zone vmap
 *************************************************/

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

/*************************************************
 * install and unisntall of wpmfs
 *************************************************/

extern void fs_now_ready(struct block_device* fs_bdev);
extern int wpmfs_init(struct super_block* sb, u64 static_area_size);
extern void wpmfs_exit(struct super_block* sb);

extern void wpmfs_print_memory_layout(struct super_block* sb,
                                      unsigned long reserved_size);
#endif /* WPMFS_WL_H */
