#include "wpmfs_wt.h"
#include <linux/fs.h>
#include <linux/string.h>
#include "pmfs.h"
#include "xip.h"

size_t _int_thres_power = MIGRATION_THRESHOLD_POWER;
size_t _cell_endur_power = CELL_ENDURANCE_POWER;

struct wt_cnter_file _wt_cnter_file;
unsigned long _pfn0;

void wpmfs_init_all_cnter() {
  memset(_wt_cnter_file.base, 0, _wt_cnter_file.size);
  // to propogate all cnters
  smp_rmb();
}

bool wt_cnter_track_fileoff(void* inode, uint64_t pageoff, uint64_t cnt) {
  loff_t isize;
  bool ret = false;
  unsigned long pfn;
  uint64_t block;
  struct inode* _inode = inode;

  /* 快速找到本应由中断控制器提供的 pfn */
  isize = i_size_read(_inode);
  if (!isize) goto out;

  block = pmfs_find_data_block(_inode, pageoff);
  if (unlikely(!block)) goto out;

  pfn = pmfs_get_pfn(_inode->i_sb, block);

  /* 更新页追踪计数器 */
  wt_cnter_track_pfn(pfn, cnt);
  ret = true;

out:
  return ret;
}

bool wt_cnter_read_fileoff(void* inode, uint64_t pageoff, uint64_t* cnt) {
  bool ret = false;
  loff_t isize;
  unsigned long pfn;
  uint64_t block;
  struct inode* _inode = inode;

  /* 快速找到本应由中断控制器提供的 pfn */
  isize = i_size_read(_inode);
  if (!isize) goto out;

  block = pmfs_find_data_block(_inode, pageoff);
  if (unlikely(!block)) goto out;

  pfn = pmfs_get_pfn(_inode->i_sb, block);

  /* 读取页追踪计数器 */
  *cnt = wt_cnter_read_pfn(pfn);
  ret = true;

out:
  return ret;
}
