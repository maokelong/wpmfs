/*
 * BRIEF DESCRIPTION
 *
 * Definitions for the PMFS filesystem.
 *
 * Copyright 2012-2013 Intel Corporation
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 * Copyright 2003 Sony Corporation
 * Copyright 2003 Matsushita Electric Industrial Co., Ltd.
 * 2003-2004 (c) MontaVista Software, Inc. , Steve Longerbeam
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */
#ifndef __PMFS_H
#define __PMFS_H

#include <linux/crc16.h>
#include <linux/mutex.h>
#include <linux/pagemap.h>
#include <linux/rcupdate.h>
#include <linux/types.h>
#include <linux/uio.h>
#include <linux/version.h>
#include <linux/pfn_t.h>
#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/string.h>

#include "pmfs_def.h"
#include "journal.h"

#define WPMFS 1
#define PAGE_SHIFT_2M 21
#define PAGE_SHIFT_1G 30

/*************************************************
 * Macros for debugging
 *************************************************/

#define PMFS_ASSERT(x)                                                      \
  if (!(x)) {                                                               \
    printk(KERN_WARNING "assertion failed %s:%d: %s\n", __FILE__, __LINE__, \
           #x);                                                             \
    dump_stack();                                                           \
  }

#ifdef pr_fmt
#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#endif

#define __FILENAME__ \
  (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

#define prt_dbg(fmt, args...)                                            \
  printk(KERN_DEBUG "wpmfs: %s @%s() #%d. " fmt, __FILENAME__, __func__, \
         __LINE__, ##args)
#define prt_err(fmt, args...)                                          \
  printk(KERN_ERR "wpmfs: %s @%s() #%d. " fmt, __FILENAME__, __func__, \
         __LINE__, ##args)
#define prt_ast(x)                                                            \
  do {                                                                        \
    if (x) break;                                                             \
    printk(KERN_ERR "wpmfs: %s @%s() #%d. Assertion failed.\n", __FILENAME__, \
           __func__, __LINE__);                                               \
    dump_stack();                                                             \
    BUG();                                                                    \
  } while (0);

/* #define pmfs_dbg(s, args...)         pr_debug(s, ## args) */
#define pmfs_dbg(s, args ...)           pr_info(s, ## args)
#define pmfs_dbg1(s, args ...)
#define pmfs_err(sb, s, args ...)       pmfs_error_mng(sb, s, ## args)
#define pmfs_warn(s, args ...)          pr_warning(s, ## args)
#define pmfs_info(s, args ...)          pr_info(s, ## args)
#define wpmfs_debug(fmt, args ...)			// 1 for ing
#define wpmfs_debug1(fmt, args ...)			prt_dbg(fmt, ## args)
#define wpmfs_error(fmt, args ...)			prt_err(fmt, ## args)
#define wpmfs_assert(x)									prt_ast(x)

extern unsigned int pmfs_dbgmask;
#define PMFS_DBGMASK_MMAPHUGE          (0x00000001)
#define PMFS_DBGMASK_MMAP4K            (0x00000002)
#define PMFS_DBGMASK_MMAPVERBOSE       (0x00000004)
#define PMFS_DBGMASK_MMAPVVERBOSE      (0x00000008)
#define PMFS_DBGMASK_VERBOSE           (0x00000010)
#define PMFS_DBGMASK_TRANSACTION       (0x00000020)
#define WPMFS_DBGMASK_INT              (0x00000100)
#define WPMFS_DBGMASK_WL_RMAP          (0x00000200)
#define WPMFS_DBGMASK_WL_VMAP          (0x00000400)
#define WPMFS_DBGMASK_WL_STRANDED      (0x00000800)

#define pmfs_dbg_mmaphuge(s, args ...)		 \
	((pmfs_dbgmask & PMFS_DBGMASK_MMAPHUGE) ? pmfs_dbg(s, args) : 0)
#define pmfs_dbg_mmap4k(s, args ...)		 \
	((pmfs_dbgmask & PMFS_DBGMASK_MMAP4K) ? pmfs_dbg(s, args) : 0)
#define pmfs_dbg_mmapv(s, args ...)		 \
	((pmfs_dbgmask & PMFS_DBGMASK_MMAPVERBOSE) ? pmfs_dbg(s, args) : 0)
#define pmfs_dbg_mmapvv(s, args ...)		 \
	((pmfs_dbgmask & PMFS_DBGMASK_MMAPVVERBOSE) ? pmfs_dbg(s, args) : 0)

#define pmfs_dbg_verbose(s, args ...)		 \
	((pmfs_dbgmask & PMFS_DBGMASK_VERBOSE) ? pmfs_dbg(s, ##args) : 0)
#define pmfs_dbg_trans(s, args ...)		 \
	((pmfs_dbgmask & PMFS_DBGMASK_TRANSACTION) ? pmfs_dbg(s, ##args) : 0)

#define wpmfs_dbg_int(s, args ...)		 \
	((pmfs_dbgmask & WPMFS_DBGMASK_INT) ? wpmfs_debug1(s, ##args) : 0)
#define wpmfs_dbg_wl_rmap(s, args ...)		 \
	((pmfs_dbgmask & WPMFS_DBGMASK_WL_RMAP) ? wpmfs_debug1(s, ##args) : 0)
#define wpmfs_dbg_wl_vmap(s, args ...)		 \
	((pmfs_dbgmask & WPMFS_DBGMASK_WL_VMAP) ? wpmfs_debug1(s, ##args) : 0)
#define wpmfs_dbg_wl_stranded(s, args ...)		 \
	((pmfs_dbgmask & WPMFS_DBGMASK_WL_STRANDED) ? wpmfs_debug1(s, ##args) : 0)

/*************************************************
 * Macros for mounting options
 *************************************************/

#define pmfs_set_bit                   __test_and_set_bit_le
#define pmfs_clear_bit                 __test_and_clear_bit_le
#define pmfs_find_next_zero_bit                find_next_zero_bit_le

#define clear_opt(o, opt)       (o &= ~PMFS_MOUNT_ ## opt)
#define set_opt(o, opt)         (o |= PMFS_MOUNT_ ## opt)
#define test_opt(sb, opt)       (PMFS_SB(sb)->s_mount_opt & PMFS_MOUNT_ ## opt)

#define PMFS_LARGE_INODE_TABLE_SIZE    (0x200000)
/* PMFS size threshold for using 2M blocks for inode table */
#define PMFS_LARGE_INODE_TABLE_THREASHOLD    (0x20000000)
/*
 * pmfs inode flags
 *
 * PMFS_EOFBLOCKS_FL	There are blocks allocated beyond eof
 */
#define PMFS_EOFBLOCKS_FL      0x20000000
/* Flags that should be inherited by new inodes from their parent. */
#define PMFS_FL_INHERITED (FS_SECRM_FL | FS_UNRM_FL | FS_COMPR_FL | \
			    FS_SYNC_FL | FS_NODUMP_FL | FS_NOATIME_FL |	\
			    FS_COMPRBLK_FL | FS_NOCOMP_FL | FS_JOURNAL_DATA_FL | \
			    FS_NOTAIL_FL | FS_DIRSYNC_FL)
/* Flags that are appropriate for regular files (all but dir-specific ones). */
#define PMFS_REG_FLMASK (~(FS_DIRSYNC_FL | FS_TOPDIR_FL))
/* Flags that are appropriate for non-directories/regular files. */
#define PMFS_OTHER_FLMASK (FS_NODUMP_FL | FS_NOATIME_FL)
#define PMFS_FL_USER_VISIBLE (FS_FL_USER_VISIBLE | PMFS_EOFBLOCKS_FL)

#define INODES_PER_BLOCK(bt) (1 << (blk_type_to_shift[bt] - PMFS_INODE_BITS))

extern unsigned int blk_type_to_shift[PMFS_BLOCK_TYPE_MAX];
extern unsigned int blk_type_to_size[PMFS_BLOCK_TYPE_MAX];

/*************************************************
 * Timing subsystem
 *************************************************/

enum timing_category {
	create_t,
	unlink_t,
	readdir_t,
	xip_read_t,
	xip_write_t,
	xip_write_fast_t,
	internal_write_t,
	memcpy_r_t,
	memcpy_w_t,
	alloc_blocks_t,
	new_trans_t,
	add_log_t,
	commit_trans_t,
	mmap_fault_t,
	fsync_t,
	free_tree_t,
	evict_inode_t,
	recovery_t,
	setup_vmap_t,
	TIMING_NUM,
};

extern const char *Timingstring[TIMING_NUM];
extern unsigned long long Timingstats[TIMING_NUM];
extern u64 Countstats[TIMING_NUM];

extern int measure_timing;
extern int support_clwb;
extern int support_clflushopt;

extern atomic64_t fsync_pages;

typedef struct timespec timing_t;

#define	INIT_TIMING(X)	timing_t X = {0}

#define PMFS_START_TIMING(name, start) \
	{if (measure_timing) getrawmonotonic(&start);}

#define PMFS_END_TIMING(name, start) \
	{if (measure_timing) { \
		INIT_TIMING(end); \
		getrawmonotonic(&end); \
		Timingstats[name] += \
			(end.tv_sec - start.tv_sec) * 1000000000 + \
			(end.tv_nsec - start.tv_nsec); \
	} \
	Countstats[name]++; \
	}

/*************************************************
 * Function Prototypes
 *************************************************/

extern void pmfs_error_mng(struct super_block *sb, const char *fmt, ...);

/* wpmfs_wl.c */
// TODO：确保每次持久化的时候都只使用 wpmfs_blockoff.blockoff
typedef union wpmfs_blockoff {
  u64 blockoff;
  struct {
    u64 val : 57;
    // 0: direct mapping
    // 1: vmap static
    // 2: vmap dynamic
    u8 vlocation : 7;
  };
} wb;
extern wb wpmfs_map_dynamic_page(struct super_block *sb, u64 blocknr);

/* file.c */
extern int pmfs_mmap(struct file *file, struct vm_area_struct *vma);

/* balloc.c */
struct allocator_factory {
  unsigned long (*pmfs_count_free_blocks)(struct super_block *sb);
  int (*pmfs_new_block)(struct super_block *sb, unsigned long *blocknr,
                        unsigned short btype, int zero);
  void (*pmfs_free_block)(struct super_block *sb, unsigned long blocknr,
                          unsigned short btype);
  void (*__pmfs_free_block)(struct super_block *sb, unsigned long blocknr,
                            unsigned short btype, void **private);
  void (*pmfs_init_blockmap)(struct super_block *sb,
                             unsigned long init_used_size);
  int (*pmfs_setup_blocknode_map)(struct super_block *sb);
};
extern struct allocator_factory Allocator;
extern bool wpmfs_select_allocator(int alloc);

// for wpmfs's simplest allocator
extern int wpmfs_new_block(struct super_block *sb, unsigned long *blocknr,
                    unsigned short btype, int zero);
extern void wpmfs_free_block(struct super_block *sb, unsigned long blocknr,
                      unsigned short btype);
extern void __wpmfs_free_block(struct super_block *sb, unsigned long blocknr,
                        unsigned short btype, void **private);
extern void wpmfs_init_blockmap(struct super_block *sb, unsigned long init_used_size);
extern int wpmfs_setup_blocknode_map(struct super_block *sb);

// for pmfs's default allocator
extern int pmfs_new_block(struct super_block *sb, unsigned long *blocknr,
                          unsigned short btype, int zero);
extern void pmfs_free_block(struct super_block *sb, unsigned long blocknr,
                            unsigned short btype);
extern void __pmfs_free_block(struct super_block *sb, unsigned long blocknr,
                              unsigned short btype, void **private);
extern void pmfs_init_blockmap(struct super_block *sb,
                               unsigned long init_used_size);
extern int pmfs_setup_blocknode_map(struct super_block *sb);
extern void pmfs_save_blocknode_mappings(struct super_block *sb);
extern struct pmfs_blocknode *pmfs_alloc_blocknode(struct super_block *sb);
extern void pmfs_free_blocknode(struct super_block *sb,
                                struct pmfs_blocknode *bnode);

/* dir.c */
extern int pmfs_add_entry(pmfs_transaction_t *trans,
		struct dentry *dentry, struct inode *inode);
extern int pmfs_remove_entry(pmfs_transaction_t *trans,
		struct dentry *dentry, struct inode *inode);

/* namei.c */
extern struct dentry *pmfs_get_parent(struct dentry *child);

/* inode.c */
extern unsigned int pmfs_free_inode_subtree(struct super_block *sb,
		__le64 root, u32 height, u32 btype, unsigned long last_blocknr);
extern int __pmfs_alloc_blocks(pmfs_transaction_t *trans,
		struct super_block *sb, struct pmfs_inode *pi,
		unsigned long file_blocknr, unsigned int num, bool zero, u8 vlocation);
extern int pmfs_init_inode_table(struct super_block *sb);
extern int pmfs_alloc_blocks(pmfs_transaction_t *trans, struct inode *inode,
		unsigned long file_blocknr, unsigned int num, bool zero, u8 vlocation);
extern u64 pmfs_find_data_block(struct inode *inode,
	unsigned long file_blocknr);
int pmfs_set_blocksize_hint(struct super_block *sb, struct pmfs_inode *pi,
		loff_t new_size);
void pmfs_setsize(struct inode *inode, loff_t newsize);

extern struct inode *pmfs_iget(struct super_block *sb, unsigned long ino);
extern void pmfs_put_inode(struct inode *inode);
extern void pmfs_evict_inode(struct inode *inode);
extern struct inode *pmfs_new_inode(pmfs_transaction_t *trans,
	struct inode *dir, umode_t mode, const struct qstr *qstr);
extern void pmfs_update_isize(struct inode *inode, struct pmfs_inode *pi);
extern void pmfs_update_nlink(struct inode *inode, struct pmfs_inode *pi);
extern void pmfs_update_time(struct inode *inode, struct pmfs_inode *pi);
extern int pmfs_write_inode(struct inode *inode, struct writeback_control *wbc);
extern void pmfs_dirty_inode(struct inode *inode, int flags);
extern int pmfs_notify_change(struct dentry *dentry, struct iattr *attr);
int pmfs_getattr(const struct path *path, struct kstat *stat,
		u32 request_mask, unsigned int flags);
extern void pmfs_set_inode_flags(struct inode *inode, struct pmfs_inode *pi);
extern void pmfs_get_inode_flags(struct inode *inode, struct pmfs_inode *pi);
extern unsigned long pmfs_find_region(struct inode *inode, loff_t *offset,
		int hole);
extern void pmfs_truncate_del(struct inode *inode);
extern void pmfs_truncate_add(struct inode *inode, u64 truncate_size);
extern void wpmfs_replace_single_datablk(struct inode *inode, pgoff_t pgoff,
                                         unsigned long blocknr);
extern u64 wpmfs_replace_tired_page(struct super_block *sb, __le64 *blk_index);

/* ioctl.c */
extern long pmfs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
#ifdef CONFIG_COMPAT
extern long pmfs_compat_ioctl(struct file *file, unsigned int cmd,
	unsigned long arg);
#endif

/* super.c */
#ifdef CONFIG_PMFS_TEST
extern struct pmfs_super_block *get_pmfs_super(void);
#endif
extern void __pmfs_free_blocknode(struct pmfs_blocknode *bnode);
extern struct super_block *pmfs_read_super(struct super_block *sb, void *data,
	int silent);
extern int pmfs_statfs(struct dentry *d, struct kstatfs *buf);
extern int pmfs_remount(struct super_block *sb, int *flags, char *data);

/* symlink.c */
extern int pmfs_block_symlink(struct inode *inode, const char *symname,
	int len);

/*************************************************
 * Inline functions
 *************************************************/

/* Mask out flags that are inappropriate for the given type of inode. */
static inline __le32 pmfs_mask_flags(umode_t mode, __le32 flags)
{
	flags &= cpu_to_le32(PMFS_FL_INHERITED);
	if (S_ISDIR(mode))
		return flags;
	else if (S_ISREG(mode))
		return flags & cpu_to_le32(PMFS_REG_FLMASK);
	else
		return flags & cpu_to_le32(PMFS_OTHER_FLMASK);
}

static inline int pmfs_calc_checksum(u8 *data, int n)
{
	u16 crc = 0;

	crc = crc16(~0, (__u8 *)data + sizeof(__le16), n - sizeof(__le16));
	if (*((__le16 *)data) == cpu_to_le16(crc))
		return 0;
	else
		return 1;
}

struct pmfs_blocknode_lowhigh {
       __le64 block_low;
       __le64 block_high;
};
               
struct pmfs_blocknode {
	struct list_head link;
	unsigned long block_low;
	unsigned long block_high;
};

struct pmfs_inode_info {
	__u32   i_dir_start_lookup;
	struct list_head i_truncated;
	struct inode	vfs_inode;
};

/*
 * WPMFS 映射内存相关信息
 */
struct wpmfs_vmap_info {
	bool enabled;

	// map staticly reserved memory to vmalloc space
  void *base_static;  
  u64 size_static;

	// map part of dynamicly allocated memory to vmalloc space
  void *base_dynamic;
  u64 num_dynamic_pages;
};

/*
 * PMFS super-block data in memory
 */
struct pmfs_sb_info {
	/*
	 * base physical and virtual address of PMFS (which is also
	 * the pointer to the super block)
	 */
	struct block_device *s_bdev;
	struct dax_device *s_dax_dev;
	phys_addr_t	phys_addr;
	void		*virt_addr;
	struct list_head block_inuse_head;
	unsigned long	block_start;
	unsigned long	block_end;
	unsigned long	num_free_blocks;
	struct mutex 	s_lock;	/* protects the SB's buffer-head */

	/*
	 * Backing store option:
	 * 1 = no load, 2 = no store,
	 * else do both
	 */
	unsigned int	pmfs_backing_option;

	/* Mount options */
	unsigned long	bpi;
	unsigned long	num_inodes;
	unsigned long	blocksize;
	unsigned long	initsize;
	unsigned long	s_mount_opt;
	kuid_t		uid;    /* Mount uid for root directory */
	kgid_t		gid;    /* Mount gid for root directory */
	umode_t		mode;   /* Mount mode for root directory */
	atomic_t	next_generation;
	/* inode tracking */
	struct mutex inode_table_mutex;
	unsigned int	s_inodes_count;  /* total inodes count (used or free) */
	unsigned int	s_free_inodes_count;    /* free inodes count */
	unsigned int	s_inodes_used_count;
	unsigned int	s_free_inode_hint;

	unsigned long num_blocknode_allocated;

	/* Journaling related structures */
	uint32_t    next_transaction_id;
	uint32_t    jsize;
	void       *journal_base_addr;
	struct mutex journal_mutex;
	struct task_struct *log_cleaner_thread;
	wait_queue_head_t  log_cleaner_wait;
	bool redo_log;

	/* truncate list related structures */
	struct list_head s_truncate;
	struct mutex s_truncate_lock;
	/* wpmfs: vmalloc space management related structure */
	struct wpmfs_vmap_info vmapi;
	
	/* wpmfs: block management related structure */
	int num_bins;
	struct list_head *block_bins;
	unsigned long	unused_block_low;
	unsigned long	unused_block_high;
};

static inline struct pmfs_sb_info *PMFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;

}
static inline struct pmfs_inode_info *PMFS_I(struct inode *inode)
{
	return container_of(inode, struct pmfs_inode_info, vfs_inode);
}

/* If this is part of a read-modify-write of the super block,
 * pmfs_memunlock_super() before calling! */
static inline struct pmfs_super_block *pmfs_get_super(struct super_block *sb)
{
  struct pmfs_sb_info *sbi = PMFS_SB(sb);

  return !sbi->vmapi.enabled ? (struct pmfs_super_block *)sbi->virt_addr
                            : (struct pmfs_super_block *)sbi->vmapi.base_static;
}

static inline pmfs_journal_t *pmfs_get_journal(struct super_block *sb)
{
	struct pmfs_super_block *ps = pmfs_get_super(sb);

	return (pmfs_journal_t *)((char *)ps +
			le64_to_cpu(ps->s_journal_offset));
}

static inline struct pmfs_inode *pmfs_get_inode_table(struct super_block *sb)
{
	struct pmfs_super_block *ps = pmfs_get_super(sb);

	return (struct pmfs_inode *)((char *)ps +
			le64_to_cpu(ps->s_inode_table_offset));
}

// seems never be used.
// static struct pmfs_super_block *pmfs_get_redund_super(struct super_block *sb)
// {
// 	struct pmfs_sb_info *sbi = PMFS_SB(sb);
// 	wpmfs_assert(0);

// 	return (struct pmfs_super_block *)(sbi->vmapi.base_static + PMFS_SB_SIZE);
// }

static void *wpmfs_get_block(struct super_block *sb, u64 blockoff) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  wb *_wb = (wb *)(&blockoff);

  switch (_wb->vlocation) {
    case 0:
		vmap_disabled:
      return _wb->val ? (void *)sbi->virt_addr + _wb->val : NULL;
    case 1:
			if(!sbi->vmapi.enabled) goto vmap_disabled;
      return _wb->val ? (void *)sbi->vmapi.base_static + _wb->val : NULL;
    case 2:
			if(!sbi->vmapi.enabled) goto vmap_disabled;
      return (void *)sbi->vmapi.base_dynamic + _wb->val;

    default:
      wpmfs_assert(0);
      return NULL;
  }
}

static wb wpmfs_get_blockoff(struct super_block *sb, u64 blocknr,
                             u8 vlocation) {
  wb wblock = {0};

  switch (vlocation) {
    case 0:
    vmap_disabled:
      wblock.val = blocknr << PAGE_SHIFT;
      wblock.vlocation = 0;
			break;

    case 2:
      // CAUTION! SOMETHING REALLY COMPLEX HAPPENED HERE
      // TODO: TRANSACTION IS REQUIRED TO GURANTEE CRASH CONSISTENCY
      if (!PMFS_SB(sb)->vmapi.enabled) goto vmap_disabled;
      wblock = wpmfs_map_dynamic_page(sb, blocknr);
			break;
  }

	wpmfs_assert(wblock.blockoff);
	return wblock;
}

extern unsigned long wpmfs_get_pfn(struct super_block *sb, u64 blockoff);

/* If this is part of a read-modify-write of the block,
 * pmfs_memunlock_block() before calling! */
static inline void *pmfs_get_block(struct super_block *sb, u64 blockoff)
{
	// struct pmfs_super_block *ps = pmfs_get_super(sb);
	struct pmfs_sb_info *sbi = PMFS_SB(sb);

	PMFS_ASSERT(blockoff);
	return blockoff ? ((void *)sbi->virt_addr + blockoff) : NULL;
}

//TODO: 更换为更通用的函数，也即 wpmfs
static inline unsigned long pmfs_get_pfn(struct super_block *sb, u64 blockoff)
{
	return (PMFS_SB(sb)->phys_addr + blockoff) >> PAGE_SHIFT;
}

static inline void *wpmfs_get_vblock(struct super_block *sb, u64 offset)
{
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
	PMFS_ASSERT(offset <= sbi->vmapi.size_static);

  return offset ? ((void *)sbi->vmapi.base_static + offset) : NULL;
}

static inline u64 wpmfs_get_blocknr(struct super_block *sb, unsigned long pfn)
{
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	return ((pfn << PAGE_SHIFT) - sbi->phys_addr) >> PAGE_SHIFT;
}

/* uses CPU instructions to atomically write up to 8 bytes */
static inline void pmfs_memcpy_atomic (void *dst, const void *src, u8 size)
{
	switch (size) {
		case 1: {
			volatile u8 *daddr = dst;
			const u8 *saddr = src;
			PM_EQU(*daddr, *saddr);
			break;
		}
		case 2: {
			volatile __le16 *daddr = dst;
			const u16 *saddr = src;
			PM_EQU(*daddr, cpu_to_le16(*saddr));
			break;
		}
		case 4: {
			volatile __le32 *daddr = dst;
			const u32 *saddr = src;
			PM_EQU(*daddr, cpu_to_le32(*saddr));
			break;
		}
		case 8: {
			volatile __le64 *daddr = dst;
			const u64 *saddr = src;
			PM_EQU(*daddr, cpu_to_le64(*saddr));
			break;
		}
		default:
			pmfs_dbg("error: memcpy_atomic called with %d bytes\n", size);
			//BUG();
	}
}

static inline void pmfs_update_time_and_size(struct inode *inode,
	struct pmfs_inode *pi)
{
	__le32 words[2];
	__le64 new_pi_size = cpu_to_le64(i_size_read(inode));

	/* pi->i_size, pi->i_ctime, and pi->i_mtime need to be atomically updated.
 	* So use cmpxchg16b here. */
	words[0] = cpu_to_le32(inode->i_ctime.tv_sec);
	words[1] = cpu_to_le32(inode->i_mtime.tv_sec);
	/* TODO: the following function assumes cmpxchg16b instruction writes
 	* 16 bytes atomically. Confirm if it is really true. */
	cmpxchg_double_local(&pi->i_size, (u64 *)&pi->i_ctime, pi->i_size,
		*(u64 *)&pi->i_ctime, new_pi_size, *(u64 *)words);
	PM_WRITE(pi->i_size);
        PM_WRITE(pi->i_ctime);
        PM_WRITE(pi->i_mtime);
}

/* assumes the length to be 4-byte aligned */
static inline void memset_nt(void *dest, uint32_t dword, size_t length)
{
	uint64_t dummy1, dummy2;
	uint64_t qword = ((uint64_t)dword << 32) | dword;

	asm volatile ("movl %%edx,%%ecx\n"
		"andl $63,%%edx\n"
		"shrl $6,%%ecx\n"
		"jz 9f\n"
		"1:      movnti %%rax,(%%rdi)\n"
		"2:      movnti %%rax,1*8(%%rdi)\n"
		"3:      movnti %%rax,2*8(%%rdi)\n"
		"4:      movnti %%rax,3*8(%%rdi)\n"
		"5:      movnti %%rax,4*8(%%rdi)\n"
		"8:      movnti %%rax,5*8(%%rdi)\n"
		"7:      movnti %%rax,6*8(%%rdi)\n"
		"8:      movnti %%rax,7*8(%%rdi)\n"
		"leaq 64(%%rdi),%%rdi\n"
		"decl %%ecx\n"
		"jnz 1b\n"
		"9:     movl %%edx,%%ecx\n"
		"andl $7,%%edx\n"
		"shrl $3,%%ecx\n"
		"jz 11f\n"
		"10:     movnti %%rax,(%%rdi)\n"
		"leaq 8(%%rdi),%%rdi\n"
		"decl %%ecx\n"
		"jnz 10b\n"
		"11:     movl %%edx,%%ecx\n"
		"shrl $2,%%ecx\n"
		"jz 12f\n"
		"movnti %%eax,(%%rdi)\n"
		"12:\n"
		: "=D"(dummy1), "=d" (dummy2) : "D" (dest), "a" (qword), "d" (length) : "memory", "rcx");
	PM_MOVNTI(dest, length, length - (length%4)); /* This macro does not perform movnti, only records */
}

static inline void memcpy_page(void *dst, void *src, bool signal_int) {
  if (dst == src) {
    wpmfs_error("dst == src.\n");
    return;
  }

  // TODO: 基于 sse2 提供的 movntdq 指令加速传输
  if (signal_int)
    PM_MEMCPY(dst, src, PAGE_SIZE);
  else
    PM_MEMCPY_NO_INT(dst, src, PAGE_SIZE);

  pmfs_flush_buffer(dst, PAGE_SIZE, true);
}

#define WPMFS_PAGE_SHIFT (4)
#define WPMFS_PAGE_USING (1 << 0)
#define WPMFS_PAGE_TIRED (1 << 1)
#define WPMFS_PAGE_VMAP (1 << 2)  // page->index available
#define WPMFS_PAGE_VMAP_DYNAMIC (1 << 3)  // dynamically allocated page?

static inline unsigned long wpmfs_page_marks(struct page* page) {
  atomic64_t* pmarks = (atomic64_t*)&page->private;
  return atomic_long_read(pmarks);
}

static inline bool wpmfs_mark_page(struct page* page, unsigned long ori_flags,
                                   unsigned long new_flags) {
  //  kernel bugs will report the empry page.
  atomic64_t* pmarks = (atomic64_t*)&page->private;
  if (atomic_long_cmpxchg(pmarks, ori_flags, new_flags) != ori_flags) {
    wpmfs_error("contention on page marks detected.\n");
    return false;
  }

  return true;
}

static inline u64 wpmfs_retrieve_datablk(struct super_block *sb, __le64 *level_ptr, unsigned int idx) {
  u64 bp = 0;
	unsigned long pfn;

  bp = le64_to_cpu(level_ptr[idx]);
  if (bp == 0) return 0;

	pfn = wpmfs_get_pfn(sb, bp);
	if (wpmfs_page_marks(pfn_to_page(pfn)) & WPMFS_PAGE_TIRED) 
		bp = wpmfs_replace_tired_page(sb, level_ptr + idx);

	return bp;	
}

static inline u64 __pmfs_find_data_block(struct super_block *sb,
		struct pmfs_inode *pi, unsigned long blocknr)
{
	__le64 *level_ptr;
	u64 bp = 0;
	u32 height, bit_shift;
	unsigned int idx;

	height = pi->height;
	bp = le64_to_cpu(pi->root);

	while (height > 0) {
		level_ptr = wpmfs_get_block(sb, bp);
		bit_shift = (height - 1) * META_BLK_SHIFT;
		idx = blocknr >> bit_shift;
		bp = wpmfs_retrieve_datablk(sb, level_ptr, idx);
		if (bp == 0)
			return 0;
		blocknr = blocknr & ((1 << bit_shift) - 1);
		height--;
	}
	return bp;
}

static inline __le64 * __wpmfs_find_pdatablk(struct super_block *sb,
		struct pmfs_inode *pi, unsigned long blocknr)
{
	__le64 *level_ptr, *ret = NULL;
	u64 bp = 0;
	u32 height, bit_shift;
	unsigned int idx;

	height = pi->height;
	bp = le64_to_cpu(pi->root);

	while (height > 0) {
		level_ptr = wpmfs_get_block(sb, bp);
		bit_shift = (height - 1) * META_BLK_SHIFT;
		idx = blocknr >> bit_shift;
		bp = le64_to_cpu(level_ptr[idx]);
		ret = &level_ptr[idx];
		if (bp == 0)
			return NULL;
		blocknr = blocknr & ((1 << bit_shift) - 1);
		height--;
	}
	return ret;
}

static inline unsigned int pmfs_inode_blk_shift (struct pmfs_inode *pi)
{
	return blk_type_to_shift[pi->i_blk_type];
}

static inline uint32_t pmfs_inode_blk_size (struct pmfs_inode *pi)
{
	return blk_type_to_size[pi->i_blk_type];
}

/* If this is part of a read-modify-write of the inode metadata,
 * pmfs_memunlock_inode() before calling! */
static inline struct pmfs_inode *pmfs_get_inode(struct super_block *sb,
						  u64	ino)
{
	struct pmfs_inode *inode_table = pmfs_get_inode_table(sb);
	u64 bp, block, ino_offset;

	if (ino == 0)
		return NULL;

	block = ino >> pmfs_inode_blk_shift(inode_table);
  bp = __pmfs_find_data_block(sb, inode_table, block);

	if (bp == 0)
		return NULL;
	ino_offset = (ino & (pmfs_inode_blk_size(inode_table) - 1));
	return (struct pmfs_inode *)(wpmfs_get_block(sb, bp) + ino_offset);
}

//TODO: 像这种没必要内联的复杂函数，还是直接丢到别的文件里去吧
static inline u64 wpmfs_reget_blockoff(struct super_block *sb, void *addr) {
  struct pmfs_sb_info *sbi = PMFS_SB(sb);
  wb blockoff = {0};

  if (!is_vmalloc_addr(addr)) {
    blockoff.val = (u64)(addr - sbi->virt_addr);
    blockoff.vlocation = 0;
    return blockoff.blockoff;
  }

  if (addr >= sbi->vmapi.base_static &&
      addr < (sbi->vmapi.base_static + sbi->vmapi.size_static)) {
    blockoff.val = (u64)(addr - sbi->vmapi.base_static);
    blockoff.vlocation = 1;
    return blockoff.blockoff;
  }

  if (addr >= sbi->vmapi.base_dynamic &&
      addr < (sbi->vmapi.base_dynamic +
              PAGE_SIZE * sbi->vmapi.num_dynamic_pages)) {
    blockoff.val = (u64)(addr - sbi->vmapi.base_dynamic);
    blockoff.vlocation = 2;
    return blockoff.blockoff;
  }

  wpmfs_assert(0);
  return blockoff.blockoff;
}

static inline u64
pmfs_get_block_off(struct super_block *sb, unsigned long blocknr,
		    unsigned short btype)
{
	return (u64)blocknr << PAGE_SHIFT;
}

static inline unsigned long
pmfs_get_numblocks(unsigned short btype)
{
	unsigned long num_blocks;

	if (btype == PMFS_BLOCK_TYPE_4K) {
		num_blocks = 1;
	} else if (btype == PMFS_BLOCK_TYPE_2M) {
		num_blocks = 512;
	} else {
		//btype == PMFS_BLOCK_TYPE_1G 
		num_blocks = 0x40000;
	}
	return num_blocks;
}

static inline unsigned long
pmfs_get_blocknr(struct super_block *sb, u64 block, unsigned short btype)
{
	return block >> PAGE_SHIFT;
}

static inline int pmfs_is_mounting(struct super_block *sb)
{
	struct pmfs_sb_info *sbi = (struct pmfs_sb_info *)sb->s_fs_info;
	return sbi->s_mount_opt & PMFS_MOUNT_MOUNTING;
}

static inline struct pmfs_inode_truncate_item * pmfs_get_truncate_item (struct 
		super_block *sb, u64 ino)
{
	struct pmfs_inode *pi = pmfs_get_inode(sb, ino);
	return (struct pmfs_inode_truncate_item *)(pi + 1);
}

static inline struct pmfs_inode_truncate_item * pmfs_get_truncate_list_head (
		struct super_block *sb)
{
	struct pmfs_inode *pi = pmfs_get_inode_table(sb);
	return (struct pmfs_inode_truncate_item *)(pi + 1);
}

static inline void check_eof_blocks(struct super_block *sb, 
		struct pmfs_inode *pi, loff_t size)
{
	if ((pi->i_flags & cpu_to_le32(PMFS_EOFBLOCKS_FL)) &&
		(size + sb->s_blocksize) > (le64_to_cpu(pi->i_blocks)
			<< sb->s_blocksize_bits))
		pi->i_flags &= cpu_to_le32(~PMFS_EOFBLOCKS_FL);
}

static inline int wpmfs_get_bin(struct super_block* sb, unsigned long blocknr) {
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	u64 blockoff = wpmfs_get_blockoff(sb, blocknr, 0).blockoff;
	u64 pfn = pmfs_get_pfn(sb, blockoff);
	int target_bin = (int)(wt_cnter_read_pfn(pfn) / get_int_thres_size());
	return (target_bin < sbi->num_bins) ? target_bin : sbi->num_bins - 1;
}

#include "wprotect.h"

/*************************************************
 * Inodes and files operations
 *************************************************/

/* dir.c */
extern const struct file_operations pmfs_dir_operations;

/* file.c */
extern const struct inode_operations pmfs_file_inode_operations;
extern const struct file_operations pmfs_xip_file_operations;
int pmfs_fsync(struct file *file, loff_t start, loff_t end, int datasync);

/* inode.c */
extern const struct address_space_operations pmfs_aops_xip;

/* bbuild.c */
void pmfs_save_blocknode_mappings(struct super_block *sb);

/* namei.c */
extern const struct inode_operations pmfs_dir_inode_operations;
extern const struct inode_operations pmfs_special_inode_operations;

/* symlink.c */
extern const struct inode_operations pmfs_symlink_inode_operations;

int pmfs_check_integrity(struct super_block *sb,
	struct pmfs_super_block *super);
void *pmfs_ioremap(struct super_block *sb, phys_addr_t phys_addr,
	ssize_t size);

int pmfs_check_dir_entry(const char *function, struct inode *dir,
			  struct pmfs_direntry *de, u8 *base,
			  unsigned long offset);

static inline int pmfs_match(int len, const char *const name,
			      struct pmfs_direntry *de)
{
	if (len == de->name_len && de->ino && !memcmp(de->name, name, len))
		return 1;
	return 0;
}

int pmfs_search_dirblock(u8 *blk_base, struct inode *dir, struct qstr *child,
			  unsigned long offset,
			  struct pmfs_direntry **res_dir,
			  struct pmfs_direntry **prev_dir);

/* pmfs_stats.c */
#define	PMFS_PRINT_TIMING	0xBCD00010
#define	PMFS_CLEAR_STATS	0xBCD00011
void pmfs_print_timing_stats(void);
void pmfs_clear_stats(void);

/* ================== Borrowed Symbols ==================== */
extern void *(*ppage_rmapping)(struct page *page);
extern bool (*ppage_vma_mapped_walk)(struct page_vma_mapped_walk *);
extern void (*prmap_walk)(struct page *, struct rmap_walk_control *);

#endif /* __PMFS_H */
