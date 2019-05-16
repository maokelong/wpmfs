#ifndef WPMFS_WL_H
#define WPMFS_WL_H

#include <linux/fs.h>
#include "pmfs.h"

extern void* ir_pmfs_sbi;

extern void fs_now_ready(void);
extern int wpmfs_init(struct super_block* sb);
extern void wpmfs_exit(void);

#endif /* WPMFS_WL_H */
