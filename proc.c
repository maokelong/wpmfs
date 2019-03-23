/*
 * wpmfs 注册的 proc 文件
 */

#include "proc.h"
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

#define BUFSIZE 128
enum opcode { OP_SYNC_CNT, OP_INVALID };
static struct proc_dir_entry *ent;

static ssize_t write_handler(struct file *file, const char __user *ubuf,
                             size_t count, loff_t *ppos) {
  char buf[BUFSIZE];
  int retval = -EINVAL, opcode, pageSN;

  // 获取并检查参数
  if (*ppos > 0 || count > BUFSIZE || copy_from_user(buf, ubuf, count)) {
    printk(KERN_ERR
           "wpmfs-proc: write handler failed to read user buffer."
           "Kernel buffer size: %d.\n",
           BUFSIZE);
    goto out;
  };
  if (sscanf(buf, "%d %d", &opcode, &pageSN) != 2 || opcode > OP_INVALID ||
      opcode < 0) {
    printk(KERN_ERR "wpmfs-proc: write handler received illegal opcode %d \n",
           opcode);
    goto out;
  }

  // 处理用户的请求
  printk(KERN_DEBUG "wpmfs-proc: opcode = %d, pageSN = %d.\n", opcode, pageSN);
  retval = 0;

out:
  return retval;
}

static ssize_t read_handler(struct file *file, char __user *ubuf, size_t count,
                            loff_t *ppos) {
  printk(KERN_DEBUG "read handler\n");
  return 0;
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
