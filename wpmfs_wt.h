/*
 * BRIEF DESCRIPTION
 *
 * Write-tracking(wt) and interrupt signaling.
 *
 */

#ifndef WPMFS_WT_H
#define WPMFS_WT_H

#include <linux/atomic.h>
#include <linux/mm.h>
#include <linux/types.h>

struct wt_cnter_info;
extern void wpmfs_init_all_cnter(void);
extern void wpmfs_int_top(unsigned long pfn);

#define wt_cnter_t atomic64_t

/*************************************************
 * page suffering threshold
 *************************************************/

/* Whenever a page suffers 2^power writes, the memory controller will sigal a
 * inetrrupt, suggesting page migraition. */
// TODO: 28
#define INTERRUPT_THRESHOLD_POWER (20)
#define CELL_ENDURANCE_POWER (INTERRUPT_THRESHOLD_POWER)

extern size_t _int_thres_power;
static inline uint64_t get_int_thres_size(void) {
  return 1 << _int_thres_power;
}
static inline uint64_t get_int_thres_mask(void) {
  return ~(get_int_thres_size() - 1);
}
extern void set_int_threshold(int power);

static inline uint64_t get_cell_idea_endurance(void) {
  return 1 << CELL_ENDURANCE_POWER;
}

/*************************************************
 * file suffering threshold
 *************************************************/

// TODO: choose suitable configs for me
#define FILE_UPDATE_THRESHOLD_POWER (INTERRUPT_THRESHOLD_POWER)

static inline uint64_t get_file_update_thes_val(void) {
  return 1 << FILE_UPDATE_THRESHOLD_POWER;
}
static inline uint64_t get_file_update_thes_mask(void) {
  return ~(get_file_update_thes_val() - 1);
}

/*************************************************
 * per-page write-tracking counters (counter file)
 *************************************************/

/* Descriptor of wt counter file. */
struct wt_cnter_file {
  wt_cnter_t* base;  // the base address
  uint64_t size;     // the total size (in bytes)
};

extern struct wt_cnter_file _wt_cnter_file;
extern unsigned long _pfn0;

static inline uint64_t wt_cnter_read_blocknr(unsigned long blocknr) {
  wt_cnter_t* pcnter = _wt_cnter_file.base + blocknr;
  return atomic_long_read(pcnter);
}

static inline uint64_t wt_cnter_read_pfn(unsigned long pfn) {
  wt_cnter_t* pcnter = _wt_cnter_file.base + pfn - _pfn0;
  return atomic_long_read(pcnter);
}

static inline uint64_t wt_cnter_read_addr(void* addr) {
  unsigned long pfn = is_vmalloc_addr(addr) ? vmalloc_to_pfn(addr)
                                            : virt_to_phys(addr) >> PAGE_SHIFT;
  wt_cnter_t* pcnter = _wt_cnter_file.base + pfn - _pfn0;
  return atomic_long_read(pcnter);
}

static inline bool _wt_cnter_add(unsigned long pfn, uint64_t cnt) {
  wt_cnter_t* pcnter = _wt_cnter_file.base + pfn - _pfn0;
  long res = atomic_long_add_return(cnt, pcnter);
  // weather signal a interrupt
  return (res & get_int_thres_mask()) ^ ((res - cnt) & get_int_thres_mask());
}

static inline void wt_cnter_track_pfn(unsigned long pfn, uint64_t cnt,
                                      bool signal_int) {
  if (_wt_cnter_add(pfn, cnt) && signal_int) {
    wpmfs_int_top(pfn);
  }
}

static inline void _wt_cnter_track_addr(void* addr, uint64_t cnt,
                                        bool signal_int) {
  // 当前访问虚拟映射内存，遍历页表找到 pte 中的 pfn
  // 否则，当前访问直接映射内存，减去 PAGE_OFFSET 就得到了物理地址
  unsigned long pfn = is_vmalloc_addr(addr) ? vmalloc_to_pfn(addr)
                                            : virt_to_phys(addr) >> PAGE_SHIFT;
  wt_cnter_track_pfn(pfn, cnt, signal_int);
}

static inline void wt_cnter_track_addr(void* addr, uint64_t cnt,
                                       bool signal_int) {
  uint64_t addr_t = (uint64_t)addr;
  if (likely((addr_t & ~PAGE_MASK) + cnt <= PAGE_SIZE)) {
    // 当该次写操作均发生在同一页
    _wt_cnter_track_addr(addr, cnt, signal_int);
  } else {
    // 当该次写操作均发生在多个页
    for (addr_t &= PAGE_MASK; addr_t < (uint64_t)addr + cnt;
         addr_t += PAGE_SIZE) {
      uint64_t sub_cnt = PAGE_SIZE;
      if (addr_t == ((uint64_t)addr & PAGE_MASK))
        sub_cnt = PAGE_SIZE - ((uint64_t)addr & ~PAGE_MASK);
      if (addr_t == (((uint64_t)addr + cnt) & PAGE_MASK))
        sub_cnt = ((uint64_t)addr + sub_cnt) & ~PAGE_MASK;

      _wt_cnter_track_addr((void*)addr_t, sub_cnt, signal_int);
    }
  }
}

extern bool wt_cnter_track_fileoff(void* inode, uint64_t pageoff, uint64_t cnt);
extern bool wt_cnter_read_fileoff(void* inode, uint64_t pageoff, uint64_t* cnt);

#endif /* WPMFS_WT_H */
