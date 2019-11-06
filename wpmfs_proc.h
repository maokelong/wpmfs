#ifndef __PROC_H
#define __PROC_H

#include <linux/init.h>

/* Commands r/w ing wt counters. For proc/ioctl. */
#define WPMFS_CMD_INC_CNT 0xBCD00020
#define WPMFS_CMD_GET_CNT 0xBCD00021
// #define WPMFS_CMD_GET_CAP 0xBCD00022

extern int __init wpmfs_init_proc(void);
extern void __exit wpmfs_destory_proc(void);

#endif /* __PROC_H */
