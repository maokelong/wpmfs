#include "wpmfs_wt.h"
#include <linux/fs.h>
#include <linux/string.h>
#include "pmfs.h"
#include "xip.h"

size_t _int_thres_power = INTERRUPT_THRESHOLD_POWER;
struct wt_cnter_file _wt_cnter_file;
unsigned long _pfn0;

void set_int_threshold(int power) {
  wpmfs_assert(power > 10 && power < 25);

  _int_thres_power = power;
  // to propogate the value of power
  smp_rmb();
}

void wpmfs_init_all_cnter() {
  memset(_wt_cnter_file.base, 0, _wt_cnter_file.size);
  // to propogate all cnters
  smp_rmb();
}

void wpmfs_inc_cnter(void* inode, struct wt_cnter_info packet) {
  loff_t isize;
  size_t error = ENODATA;
  unsigned long pfn;
  uint64_t block;
  struct inode* _inode = inode;

  /* 快速找到本应由中断控制器提供的 pfn */
  isize = i_size_read(_inode);
  if (!isize) goto out;

  block = pmfs_find_data_block(_inode, packet.pageoff);
  if (unlikely(!block)) goto out;

  pfn = pmfs_get_pfn(_inode->i_sb, block);

  /* 更新页追踪计数器 */
  wt_cnter_add_int_pfn(pfn, packet.cnt);
  wpmfs_debug("cnter for %llu now reads %llu", packet.pageoff,
              _wt_cnter_read(pfn));
  error = 0;

out:
  if (error) wpmfs_error("");
}

void wpmfs_get_cnter(void* inode, struct wt_cnter_info* packet) {
  loff_t isize;
  size_t error = ENODATA;
  unsigned long pfn;
  uint64_t block;
  struct inode* _inode = inode;

  /* 快速找到本应由中断控制器提供的 pfn */
  isize = i_size_read(_inode);
  if (!isize) goto out;

  block = pmfs_find_data_block(_inode, packet->pageoff);
  if (unlikely(!block)) goto out;

  pfn = pmfs_get_pfn(_inode->i_sb, block);

  /* 读取页追踪计数器 */
  packet->cnt = _wt_cnter_read(pfn);
  error = 0;

out:
  if (error) wpmfs_error("");
}

void wpmfs_int_top(unsigned long pfn) {
  // TODO
  wpmfs_debug1("pfn = %lu, cnter = %llu", pfn, _wt_cnter_read(pfn));
}
