#ifndef __PROC_H
#define __PROC_H

#include <linux/init.h>

extern int __init wpmfs_init_proc(void);
extern void __exit wpmfs_destory_proc(void);

#endif /* __PROC_H */

