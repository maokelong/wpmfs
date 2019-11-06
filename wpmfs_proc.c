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
      uint64_t pageoff;  // 被统计页起始位置相对于文件的偏移
      uint64_t cnt;      // 被统计页的写次数（读写取决于命令）
    };
    // uint64_t capacity;
  };
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
      fileUsr = fget_raw(message.fd);
      if (!fileUsr) {
        wpmfs_error("Failed to open target file. fd = %d.\n", message.fd);
        goto out;
      }
      wt_cnter_track_fileoff(fileUsr->f_inode, message.pageoff, message.cnt);
      wpmfs_debug("fd = %d, pageoff = %llu, cnt = %llu.\n", message.fd,
                  message.pageoff, message.cnt);
      break;

    // To read the write-tracking counter for page at given fileoff
    case WPMFS_CMD_GET_CNT:
      fileUsr = fget_raw(message.fd);
      if (!fileUsr) {
        wpmfs_error("Failed to open target file. fd = %d.\n", message.fd);
        goto out;
      }
      wt_cnter_read_fileoff(fileUsr->f_inode, message.pageoff, &message.cnt);
      break;

      // To get the capacity of the whole filesystem (in bytes).
      // case WPMFS_CMD_GET_CAP:
      //   break;

    default:
      wpmfs_error("Received invalid request: opcode = 0x%x.\n", message.opcode);
      goto out;
  }

  /* 成功，释放文件指针 */
  retval = count;  // 表示成功「写入」的长度
  fput(fileUsr);

out:
  return retval;
}

static ssize_t read_handler(struct file *file, char __user *ubuf, size_t count,
                            loff_t *ppos) {
  char buf[BUFSIZE];
  int retval = -EINVAL, len;

  /* 传输页写次数给应用 */
  switch (message.opcode) {
    case WPMFS_CMD_GET_CNT:
      len = sprintf(buf, "%llu\n", message.cnt);
      if (copy_to_user(ubuf, buf, len)) {
        wpmfs_error("Failed to fill in user buffer.");
        goto out;
      }
      wpmfs_debug("fd = %d, pageoff = %llu, cnt = %llu.\n", message.fd,
                  message.pageoff, message.cnt);
      break;

      // case WPMFS_CMD_GET_CAP:
      //   break;

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
