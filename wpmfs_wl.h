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

struct wpmfs_mptables {
  struct {
    __le64 num_pages;
  } mptable_static;

  struct {
    __le64 head_blocknr;
  } mptable_dynamic;
};

static inline struct wpmfs_mptables* wpmfs_get_mptables(
    struct super_block* sb) {
  struct pmfs_sb_info* sbi = PMFS_SB(sb);
  return (struct wpmfs_mptables*)(sbi->virt_addr);
}

static inline mptable_slot_t* wpmfs_get_mptable_static(struct super_block* sb) {
  return (mptable_slot_t*)(wpmfs_get_mptables(sb) + 1);
}

static inline mptable_slot_t* wpmfs_get_mptable_dynamic(
    struct super_block* sb) {
  u64 blocknr =
      le64_to_cpu(wpmfs_get_mptables(sb)->mptable_dynamic.head_blocknr);
  u64 blockoff = wpmfs_get_blockoff(sb, blocknr, 0).blockoff;
  return (mptable_slot_t*)wpmfs_get_block(sb, blockoff);
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
extern int wpmfs_init(struct super_block *sb, u64 *reserved_memory_size);
extern int wpmfs_recv(struct super_block *sb);
extern void wpmfs_exit(struct super_block* sb);

extern void wpmfs_print_wl_switch(struct super_block* sb);
extern bool wpmfs_wl_stranded_enabled(void);
extern void wpmfs_print_memory_layout(struct super_block* sb,
                                      unsigned long reserved_size);
#endif /* WPMFS_WL_H */
