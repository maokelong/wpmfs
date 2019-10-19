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
extern void wpmfs_inc_cnter(void* inode, struct wt_cnter_info packet);
extern void wpmfs_get_cnter(void* inode, struct wt_cnter_info* packet);
extern void wpmfs_int_top(unsigned long pfn);

#define wt_cnter_t atomic64_t

/*************************************************
 * page suffering threshold
 *************************************************/

/* Whenever a page suffers 2^power writes, the memory controller will sigal a
 * inetrrupt, suggesting page migraition. */
// TODO: 28
#define INTERRUPT_THRESHOLD_POWER (13)
#define CELL_ENDURANCE_POWER (13)

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
#define FILE_UPDATE_THRESHOLD_POWER (3)
#define FILE_UPDATE_CNT_MAJOR (100)

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

/* Commands r/w ing wt counters. For proc/ioctl. */
#define WPMFS_CMD_INC_CNT 0xBCD00020
#define WPMFS_CMD_GET_CNT 0xBCD00021

/* The information of the filepage-corrleated wt counter. */
struct wt_cnter_info {
  uint64_t pageoff;  // 被统计页起始位置相对于文件的偏移
  uint64_t cnt;      // 被统计页的写次数（读写取决于命令）
};

static inline uint64_t wt_cnter_read(unsigned long blocknr) {
  wt_cnter_t* pcnter = _wt_cnter_file.base + blocknr + _pfn0;
  return atomic_long_read(pcnter);
}

static inline uint64_t _wt_cnter_read(unsigned long pfn) {
  wt_cnter_t* pcnter = _wt_cnter_file.base + pfn - _pfn0;
  return atomic_long_read(pcnter);
}

static inline uint64_t _wt_cnter_read_addr(void* addr) {
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

static inline void wt_cnter_add_int_pfn(unsigned long pfn, uint64_t cnt) {
  if (_wt_cnter_add(pfn, cnt)) wpmfs_int_top(pfn);
}

static inline void _wt_cnter_add_int_addr(void* addr, uint64_t cnt) {
  // 当前访问虚拟映射内存，遍历页表找到 pte 中的 pfn
  // 否则，当前访问直接映射内存，减去 PAGE_OFFSET 就得到了物理地址
  unsigned long pfn = is_vmalloc_addr(addr) ? vmalloc_to_pfn(addr)
                                            : virt_to_phys(addr) >> PAGE_SHIFT;
  wt_cnter_add_int_pfn(pfn, cnt);
}

static inline void wt_cnter_add_int_addr(void* addr, uint64_t cnt) {
  uint64_t addr_t = (uint64_t)addr;
  if (likely((addr_t & ~PAGE_MASK) + cnt <= PAGE_SIZE)) {
    // 当该次写操作均发生在同一页
    _wt_cnter_add_int_addr(addr, cnt);
  } else {
    // 当该次写操作均发生在多个页
    for (addr_t &= PAGE_MASK; addr_t < (uint64_t)addr + cnt;
         addr_t += PAGE_SIZE) {
      uint64_t sub_cnt = PAGE_SIZE;
      if (addr_t == ((uint64_t)addr & PAGE_MASK))
        sub_cnt = PAGE_SIZE - ((uint64_t)addr & ~PAGE_MASK);
      if (addr_t == (((uint64_t)addr + cnt) & PAGE_MASK))
        sub_cnt = ((uint64_t)addr + sub_cnt) & ~PAGE_MASK;

      _wt_cnter_add_int_addr((void*)addr_t, sub_cnt);
    }
  }
}

/*************************************************
 * per-inode update-tracking counter (i_private)
 *************************************************/

// we dont need to initialize the `inode->i_private` since 
// `inode_init_always` will do that for us

// i_private: &inode->i_private, used as a cnter
// minor: if only replace a single datablock
// return true if a file has suffered too many updates
static inline bool wt_file_updated(void** i_private, bool minor) {
  wt_cnter_t* pcnter;
  long res, cnt;

  if (!i_private) return false;
  pcnter = *(wt_cnter_t**)i_private;
  cnt = minor ? 1 : FILE_UPDATE_CNT_MAJOR;
  res = atomic_long_add_return(cnt, pcnter);
  return (res & get_file_update_thes_val()) ^
         ((res - cnt) & get_file_update_thes_mask());
}

#endif /* WPMFS_WT_H */
