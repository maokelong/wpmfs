#include "wpmfs_wl.h"
#include <linux/string.h>

void* ir_pmfs_sbi;

int wpmfs_memory_init(struct super_block* sb) {
  wpmfs_debug1("memory init...");
  return 0;
}
