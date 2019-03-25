/*
 * wpmfs 注册的 proc 文件
 */

#include "proc.h"
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

#define BUFSIZE 128
enum opcode { OP_INVALID };
static struct proc_dir_entry *ent;

static ssize_t write_handler(struct file *file, const char __user *ubuf,
                             size_t count, loff_t *ppos) {
  char buf[BUFSIZE];
  int retval = -EINVAL;
  retval = 0;

out:
  return retval;
}

static ssize_t read_handler(struct file *file, char __user *ubuf, size_t count,
                            loff_t *ppos) {
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
