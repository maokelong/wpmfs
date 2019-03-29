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
#include "wpmfs_wr.h"

#define BUFSIZE 128
static struct proc_dir_entry *ent;

struct {
  int fd;
  unsigned opcode;
  struct wpmfs_sync_cnt packet;
} args;

// 参数格式：fd opcode pageoff (cnt) \n
// 也即，文件描述符，操作码，页偏移，写次数，换行符
static ssize_t write_handler(struct file *file, const char __user *ubuf,
                             size_t count, loff_t *ppos) {
  char buf[BUFSIZE];
  struct file *fileUsr = NULL;
  int retval = -EINVAL, argc;

  /* 获取并检查参数 */
  if (count >= BUFSIZE || copy_from_user(buf, ubuf, count)) {
    wpmfs_error(
        "Write handler failed to read user buffer. "
        "Kernel buffer size: %d.\n",
        BUFSIZE);
    goto out;
  };
  buf[count] = '\0';  // 以免应用未传递额外控制字符
  wpmfs_debug("Kbuf reads: %s.\n", buf);
  argc = sscanf(buf, "%d %u %u %u", &args.fd, &args.opcode,
                &args.packet.pageoff, &args.packet.cnt);
  if (!((argc == 3 && args.opcode == WPMFS_GET_CNT) ||
        (argc == 4 && args.opcode == WPMFS_INC_CNT)) ||
      args.fd < 0) {
    wpmfs_error(
        "Write handler received illegal request. "
        "Of which, argc = %d, opcode = 0x%x.\n",
        argc, args.opcode);
    goto out;
  }

  /* 获取文件指针 */
  fileUsr = fget_raw(args.fd);
  if (!fileUsr) {
    wpmfs_error("Write handler cannot open a file using given fd = %d.\n",
                args.fd);
    goto out;
  }

  /* 处理用户请求 */
  switch (args.opcode) {
    case WPMFS_INC_CNT:
      wpmfs_inc_cnt(fileUsr->f_inode, args.packet);
      break;

    case WPMFS_GET_CNT:
      wpmfs_get_cnt(fileUsr->f_inode, &args.packet);
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

  /* 检查应用缓冲区是否足够大 */
  wpmfs_debug("Ubuf len = %lu.\n", count);
  len = sprintf(buf, "%u", args.packet.cnt);
  wpmfs_assert(len < BUFSIZE);  //正确的设计下不会发生数组越界
  if (count < len) {
    wpmfs_error(
        "Read handler failed to fill user buffer."
        "Requested size: %d.\n",
        len);
    goto out;
  }

  /* 传输页写次数给应用 */
  len = sprintf(buf, "%u\n", args.packet.cnt);
  if (copy_to_user(ubuf, buf, len)) {
    wpmfs_error("Read handler failed to fill user buffer.");
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
