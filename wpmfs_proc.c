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
  int fd;
  unsigned opcode;
  struct wt_cnter_info packet;
} message;
#pragma pack()

// 参数格式：fd opcode pageoff (cnt) \n
// 也即，文件描述符，操作码，页偏移，写次数，换行符
static ssize_t write_handler(struct file *file, const char __user *ubuf,
                             size_t count, loff_t *ppos) {
  struct file *fileUsr = NULL;
  int retval = -EINVAL;

  /* 获取并检查参数 */
  if (count != sizeof(message) &&
      count != sizeof(message) - sizeof(message.packet.cnt)) {
    wpmfs_error(
        "Received illegal message, of which size = %lu. Note that only %lu and "
        "%lu are expected.\n",
        count, sizeof(message), sizeof(message) - sizeof(message.packet.cnt));
    goto out;
  }
  if (copy_from_user(&message, ubuf, count)) {
    wpmfs_error("Failed to read user buffer.\n");
    goto out;
  };
  if (message.fd < 0 || (message.opcode != WPMFS_CMD_GET_CNT &&
                         message.opcode != WPMFS_CMD_INC_CNT)) {
    wpmfs_error(
        "Received invalid request, "
        "of which, fd = %d, opcode = 0x%x.\n",
        message.fd, message.opcode);
    fput(fileUsr);
    goto out;
  }

  /* 获取应用内存池指针 */
  fileUsr = fget_raw(message.fd);
  if (!fileUsr) {
    wpmfs_error("Failed to open target file. fd = %d.\n", message.fd);
    goto out;
  }

  wpmfs_debug(
      "fd = %d, opcode = 0x%x, pageoff = %u, cnt = %u. Message size = %lu.\n",
      message.fd, message.opcode, message.packet.pageoff, message.packet.cnt,
      count);

  /* 处理用户请求 */
  switch (message.opcode) {
    case WPMFS_CMD_INC_CNT:
      wpmfs_inc_cnter(fileUsr->f_inode, message.packet);
      break;

    case WPMFS_CMD_GET_CNT:
      wpmfs_get_cnter(fileUsr->f_inode, &message.packet);
      break;

    default:
      // 检查做得正确的话不可能到达这个分支
      wpmfs_assert(0);
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

  /* 检查应用是否提供了正确的缓冲区 */
  if (count != sizeof(message.packet.cnt)) {
    wpmfs_error("Illegal user buffer, of which size = %lu.\n", count);
    goto out;
  }

  /* 传输页写次数给应用 */
  len = sprintf(buf, "%llu\n", message.packet.cnt);
  if (copy_to_user(ubuf, buf, len)) {
    wpmfs_error("Failed to fill in user buffer.");
    goto out;
  }

  /* 成功 */
  retval = 0;  // 表示文件已经读至结尾

out:
  return retval;
}

static struct file_operations proc_ops = {
    .owner = THIS_MODULE,
    .write = write_handler,
    .read = read_handler,
};

int __init init_proc(void) {
  ent = proc_create("wpmfs_proc", 0660, NULL, &proc_ops);
  return 0;
}

void __exit destory_proc(void) { proc_remove(ent); }
