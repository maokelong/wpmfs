/*
 * BRIEF DESCRIPTION
 *
 * Write-tracking(wt) for wpmfs.
 *
 */

#ifndef WPMFS_WT_H
#define WPMFS_WT_H

#include <linux/atomic.h>
#include <linux/types.h>
#define wt_cnter_t atomic_long_t

/* Whenever a page suffers 2^power writes, the memory controller will sigal a
 * inetrrupt, suggesting page migraition. */
#define INTERRUPT_THRESHOLD_POWER (28)

extern size_t _int_thres_power;
static inline uint64_t get_int_thres_size(void) {
  return 1 << _int_thres_power;
}
static inline uint64_t get_int_thres_mask(void) {
  return get_int_thres_size() - 1;
}
extern void set_int_threshold(int power);

/* Descriptor of wt counter file. */
struct wt_cnter_file {
  wt_cnter_t* base;  // the base address
  uint64_t size;     // the total size (in bytes)
};

extern struct wt_cnter_file _wt_cnter_file;
extern unsigned long _pfn0;

/* Commands r/w ing wt counters. For proc/ioctl. */
#define WPMFS_CMD_INC_CNT 0xBCD00020
#define WPMFS_CMD_GET_CNT 0xBCD00021

/* The information of the filepage-corrleated wt counter. */
struct wt_cnter_info {
  uint64_t pageoff;  // 被统计页起始位置相对于文件的偏移
  uint64_t cnt;      // 被统计页的写次数（读写取决于命令）
};

static inline uint64_t wt_cnter_read(unsigned long pfn) {
  wt_cnter_t* pcnter = _wt_cnter_file.base + pfn - _pfn0;
  return atomic_long_read(pcnter);
}

static inline bool wt_cnter_add(unsigned long pfn, uint64_t cnt) {
  wt_cnter_t* pcnter = _wt_cnter_file.base + pfn - _pfn0;
  long res = atomic_long_add_return(cnt, pcnter);
  // weather signal a interrupt
  return (res & ~get_int_thres_mask()) ^ ((res - cnt) & ~get_int_thres_mask());
}

extern void wpmfs_init_all_cnter(void);
extern void wpmfs_inc_cnter(void* inode, struct wt_cnter_info packet);
extern void wpmfs_get_cnter(void* inode, struct wt_cnter_info* packet);

#endif /* WPMFS_WT_H */
