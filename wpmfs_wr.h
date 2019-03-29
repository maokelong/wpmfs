#ifndef WPMFS_WR_H
#define WPMFS_WR_H

#include <linux/fs.h>
#include <linux/types.h>

/* wpmfs_wr.c */
#define WPMFS_INC_CNT 0xBCD00020
#define WPMFS_GET_CNT 0xBCD00021

struct wpmfs_sync_cnt {
  u_int32_t pageoff;  // 被统计页起始位置相对于文件的偏移
  u_int32_t cnt;      // 被统计页的写次数（读写取决于命令）
};

extern void wpmfs_inc_cnt(struct inode* inode, struct wpmfs_sync_cnt packet);
extern void wpmfs_get_cnt(struct inode* inode, struct wpmfs_sync_cnt* packet);

#endif /* WPMFS_WR_H */
