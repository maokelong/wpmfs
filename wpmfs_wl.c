#include "wpmfs_wl.h"
#include <linux/kernel.h>
#include <linux/kfifo.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include "wpmfs_wt.h"

enum AREA_OF_PFN { AREA_VMAP, AREA_APP, AREA_DIRECT, AREA_INVALID };

void* ir_pmfs_sbi;

/**
 * struct int_ctrl - the interrupt controller
 * @fs_ready: if fs was mounted successfully
 * @fifo: a queue for storing pfn
 * @fifo_lock: protect access to pfn queue
 * @work: should bind to the bottom half
 * @workqueue: a private workqueue run works
 *
 * This struct provides communication channels for the top half, to temporally
 * store pfns of hot pages and shedule the bottom half.
 */
struct int_ctrl {
  bool fs_ready;

  struct kfifo fifo;
  spinlock_t fifo_lock;

  struct work_struct work;
  struct workqueue_struct* workqueue;
};

struct int_ctrl _int_ctrl = {
    .fs_ready = false,
    .workqueue = NULL,
};

void fs_now_ready(void) { _int_ctrl.fs_ready = true; }

static bool _fetch_int_requests(unsigned long* ppfn) {
  // TODO: 这里使用最简单的请求调度算法，不对页迁移请求进行任何合并操作
  return kfifo_out_spinlocked(&_int_ctrl.fifo, ppfn, sizeof(*ppfn),
                              &_int_ctrl.fifo_lock);
}

static bool _check_vmap(unsigned long pfn) {
  // TODO:
  return true;
}

static bool _check_app(unsigned long pfn) {
  // TODO:
  return true;
}

static bool _check_direct(unsigned long pfn) {
  // TODO:
  return true;
}

static enum AREA_OF_PFN area_of_pfn(unsigned long pfn) {
  if (_check_vmap(pfn))
    return AREA_VMAP;
  else if (_check_app(pfn))
    return AREA_APP;
  else if (_check_direct(pfn))
    return AREA_DIRECT;
  else
    return AREA_INVALID;
}

static void _level_area_vmap(unsigned long pfn) {
  // TODO:
}

static void _level_area_app(unsigned long pfn) {
  // TODO:
}

static void _level_area_direct(unsigned long pfn) {
  // TODO:
}

static void _wear_lerveling(unsigned long pfn) {
  switch (area_of_pfn(pfn)) {
    case AREA_VMAP:
      _level_area_vmap(pfn);
      break;

    case AREA_APP:
      _level_area_app(pfn);
      break;

    case AREA_DIRECT:
      _level_area_direct(pfn);
      break;

    default:
      wpmfs_assert(0);
      break;
  }
}

static void _int_bottom(struct work_struct* work) {
  unsigned long pfn;
  wpmfs_debug("we've reached the bottom half!");
  if (!_fetch_int_requests(&pfn)) return;
  _wear_lerveling(pfn);
  wpmfs_debug("and goes on! Dealing pfn = %lu\n", pfn);
}

void wpmfs_int_top(unsigned long pfn) {
  /* Check if filesytem has been established */
  if (!_int_ctrl.fs_ready) return;
  wpmfs_assert(_int_ctrl.workqueue);
  wpmfs_debug("pfn = %lu, cnter = %llu", pfn, _wt_cnter_read(pfn));

  /* Enqueue to the FIFO */
  if (kfifo_in_spinlocked(&_int_ctrl.fifo, &pfn, sizeof(pfn),
                          &_int_ctrl.fifo_lock))
    /* Schedule the bottom handler */
    queue_work(_int_ctrl.workqueue, &_int_ctrl.work);
}

static int _init_int(struct super_block* sb) {
  int rc = 0;
  /* Init spinlock and kfifo */
  rc = kfifo_alloc(&_int_ctrl.fifo, sizeof(unsigned long) * (1 << 10),
                   GFP_KERNEL);
  if (rc) {
    wpmfs_error("Cannot create kfifo\n");
    return -ENOMEM;
  }

  spin_lock_init(&_int_ctrl.fifo_lock);

  /* Create a workqueue to avoid causing delays for other queue users. */
  _int_ctrl.workqueue = create_singlethread_workqueue("my_workqueue");
  if (!_int_ctrl.workqueue) {
    wpmfs_error("Cannot create workqueue\n");
    return -ENOMEM;
  }

  /* Init work */
  INIT_WORK(&_int_ctrl.work, _int_bottom);
  return rc;
}

static int _init_mem(struct super_block* sb) {
  // 加载静态区内存
  // TODO
  return 0;
}

static void _exit_int(void) {
  /* Safely destroy a workqueue. */
  if (_int_ctrl.workqueue) destroy_workqueue(_int_ctrl.workqueue);
  kfifo_free(&_int_ctrl.fifo);
}

int wpmfs_init(struct super_block* sb) {
  // TODO: to replace pmfs_init
  // TODO: when not to init a new fs instance
  _init_int(sb);
  _init_mem(sb);
  return 0;
}

void wpmfs_exit(void) { _exit_int(); }
