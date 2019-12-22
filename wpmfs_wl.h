#ifndef WPMFS_WL_H
#define WPMFS_WL_H

#include <linux/fs.h>
#include "pmfs.h"

/*************************************************
 * descriptor and operations for mapping table
 *************************************************/

typedef struct wpmfs_pgtable_slot {
  __le64 blocknr;
} mptable_slot_t;

// m4m, i.e. mapping table for mapping table
typedef struct wpmfs_m4m_slot {
  __le64 frag_blocknr;
} m4m_slot_t;

struct wpmfs_mptable_meta {
  __le64 num_prealloc_pages;
  __le64 mptable_blocknr;
};

static inline int frag_mptable_slots(void) {
  return PAGE_SIZE / sizeof(mptable_slot_t);
}

static inline struct wpmfs_mptable_meta* wpmfs_get_mptable_meta(
    struct super_block* sb) {
  struct pmfs_sb_info* sbi = PMFS_SB(sb);
  return (struct wpmfs_mptable_meta*)(sbi->virt_addr);
}

static inline mptable_slot_t* wpmfs_get_mptable_dynamic(
    struct super_block* sb) {
  u64 blocknr = le64_to_cpu(wpmfs_get_mptable_meta(sb)->mptable_blocknr);
  u64 blockoff = wpmfs_get_blockoff(sb, blocknr, 0).blockoff;
  return (mptable_slot_t*)wpmfs_get_block(sb, blockoff);
}

static inline m4m_slot_t* wpmfs_get_m4m(struct super_block* sb,
                                        u64* num_slots) {
  struct wpmfs_mptable_meta* mptable = wpmfs_get_mptable_meta(sb);
  if (num_slots)
    *num_slots = (le64_to_cpu(mptable->num_prealloc_pages) +
                  (frag_mptable_slots() - 1)) /
                 frag_mptable_slots();
  return (m4m_slot_t*)(mptable + 1);
}

static inline m4m_slot_t* wpmfs_get_m4m_slot(struct super_block* sb,
                                             pgoff_t index) {
  return wpmfs_get_m4m(sb, NULL) + (index / frag_mptable_slots());
}

static inline mptable_slot_t* wpmfs_get_pgtable_slot(struct super_block* sb,
                                                     pgoff_t index) {
  u64 blocknr = le64_to_cpu(wpmfs_get_m4m_slot(sb, index)->frag_blocknr);
  u64 blockoff = pmfs_get_block_off(sb, blocknr, PMFS_BLOCK_TYPE_4K);

  return (mptable_slot_t*)pmfs_get_block(sb, blockoff) +
         index % frag_mptable_slots();
}

/*************************************************
 * exchange information with the proc file
 *************************************************/

extern u64 wpmfs_get_capacity(void);
extern bool wpmfs_get_fs_wear(unsigned long blocknr, u64* wear_times);

/*************************************************
 * install and unisntall of wpmfs
 *************************************************/

extern void fs_now_ready(struct block_device* fs_bdev);
extern void wpmfs_set_wl_switch(int wlsw);
extern int wpmfs_init_hard(struct super_block* sb, u64* reserved_memory_size);
extern int wpmfs_init_soft(struct super_block* sb);
extern void wpmfs_exit(struct super_block* sb);

extern void wpmfs_print_wl_switch(struct super_block* sb);
extern bool wpmfs_wl_stranded_enabled(void);
extern void wpmfs_print_memory_layout(struct super_block* sb,
                                      unsigned long reserved_size);
#endif /* WPMFS_WL_H */
