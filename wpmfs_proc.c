/*
 * wpmfs 注册的 proc 文件
 */


#include "wpmfs_proc.h"
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include "pmfs.h"
#include "wpmfs_wl.h"
#include "wpmfs_wt.h"

#define BUFSIZE 128
static struct proc_dir_entry *ent;

#pragma pack(1)  // 统一不加 padding
struct {
  unsigned opcode;

  union {
    struct {
      int fd;
      uint64_t pageoff;    // 被统计页起始位置相对于文件的偏移
      uint64_t page_wcnt;  // 被统计页的写次数（读写取决于命令）
    };

    uint64_t capacity;

    struct {
      uint64_t blocknr;
      uint64_t blk_wcnt;
    };
  };

  uint8_t succ;
} message;
#pragma pack()

// 参数格式：fd opcode pageoff (cnt) \n
// 也即，文件描述符，操作码，页偏移，写次数，换行符
static ssize_t write_handler(struct file *file, const char __user *ubuf,
                             size_t count, loff_t *ppos) {
  struct file *fileUsr = NULL;
  int retval = -EINVAL;

  /* 获取并检查参数 */
  if (copy_from_user(&message, ubuf, count)) {
    wpmfs_error("Failed to read user buffer.\n");
    goto out;
  };

  /* 处理用户请求 */
  switch (message.opcode) {
    // To update the write-tracking counter for page at given fileoff
    case WPMFS_CMD_INC_CNT:
      wpmfs_debug("fd = %d, pageoff = %llu, cnt = %llu.\n", message.fd,
                  message.pageoff, message.page_wcnt);

      fileUsr = fget_raw(message.fd);
      if (!fileUsr) {
        wpmfs_error("File for fd %d not exist.\n", message.fd);
        goto out;
      }

      message.succ = wt_cnter_track_fileoff(fileUsr->f_inode, message.pageoff,
                                            message.page_wcnt);
      if (!message.succ)
        wpmfs_error("pageoff %llu too large.\n", message.pageoff);

      break;

    // To read the write-tracking counter for page at given fileoff
    case WPMFS_CMD_GET_CNT:
      wpmfs_debug("fd = %d, pageoff = %llu, cnt = %llu.\n", message.fd,
                  message.pageoff, message.page_wcnt);

      fileUsr = fget_raw(message.fd);
      if (!fileUsr) {
        wpmfs_error("File for fd %d not exist.\n", message.fd);
        goto out;
      }

      message.succ = wt_cnter_read_fileoff(fileUsr->f_inode, message.pageoff,
                                           &message.page_wcnt);
      wpmfs_debug("fd = %d, pageoff = %llu, cnt = %llu.\n", message.fd,
                  message.pageoff, message.page_wcnt);

      if (!message.succ)
        wpmfs_error("pageoff %llu too large.\n", message.pageoff);
      break;

    // To get the capacity of the whole filesystem (in bytes).
    case WPMFS_CMD_GET_FS_CAP:
      message.capacity = wpmfs_get_capacity();
      message.succ = message.capacity != 0;

      break;

    case WPMFS_CMD_GET_FS_WEAR:
      message.succ = wpmfs_get_fs_wear(message.blocknr, &message.blk_wcnt);
      if (!message.succ)
        wpmfs_error("blocknr %llu too large.\n", message.blocknr);
      break;

    default:
      wpmfs_error("Received invalid request: opcode = 0x%x.\n", message.opcode);
      goto out;
  }

  /* 成功，释放文件指针 */
  retval = count;  // 表示成功「写入」的长度
  if (fileUsr) fput(fileUsr);

out:
  return retval;
}

static ssize_t read_handler(struct file *file, char __user *ubuf, size_t count,
                            loff_t *ppos) {
  int retval = -EINVAL;

  if (!message.succ) goto out;

  /* 传输页写次数给应用 */
  switch (message.opcode) {
    case WPMFS_CMD_GET_CNT:
    case WPMFS_CMD_GET_FS_CAP:
    case WPMFS_CMD_GET_FS_WEAR:
      if (copy_to_user(ubuf, &message, sizeof(message)))
        wpmfs_error("Failed to fill in user buffer.\n");
      break;

    default:
      wpmfs_error("Confusing read request: opcode = 0x%x.\n", message.opcode);
      goto out;
  }

  retval = 0;  // 表示文件已经读至结尾

out:
  return retval;
}

static struct file_operations proc_ops = {
    .owner = THIS_MODULE,
    .write = write_handler,
    .read = read_handler,
};

int __init wpmfs_init_proc(void) {
  ent = proc_create("wpmfs_proc", 0660, NULL, &proc_ops);
  return 0;
}

void __exit wpmfs_destory_proc(void) { proc_remove(ent); }
