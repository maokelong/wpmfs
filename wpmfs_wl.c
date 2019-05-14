#include "wpmfs_wl.h"
#include <linux/string.h>
#include "wpmfs_wt.h"

void* ir_pmfs_sbi;

static int __init wpmfs_init_int(struct super_block* sb) {
  // 加载中断控制代码
  // TODO
  return 0;
}

static int __init wpmfs_init_mem(struct super_block* sb) {
  // 加载静态区内存
  // TODO
  return 0;
}

int __init wpmfs_init(struct super_block* sb) {
  wpmfs_debug1("memory init...");
  wpmfs_init_int(sb);
  wpmfs_init_mem(sb);
  return 0;
}
