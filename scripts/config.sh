#!/bin/sh

CONFIG_NAME_FS="wpmfs"
CONFIG_NAME_PROC="wpmfs_proc"

CONFIG_PATH_FS="/mnt/$CONFIG_NAME_FS"
CONFIG_PATH_PROC="/proc/$CONFIG_NAME_PROC"
CONFIG_PATH_PMEM_DEV="/dev/pmem0"

# pmfs raw
CONFIG_FS_INIT_HARD=1 # 1/0
CONFIG_FS_DBGMASK=$((0x00000000))

CONFIG_FS_TIMING=1 # 1/0
CONFIG_FS_ENABLE_TRACKING=1 # 1/0
CONFIG_FS_ENABLE_VMAP=1 # 1/0
CONFIG_FS_ALLOCATOR=0 # 0 for wpmfs; 1 for pmfs
CONFIG_FS_WL_SWITCH=7 # 1 for rmap; 2 for vmap; 4 for stranded
CONFIG_FS_INT_THRES=17 # in power
CONFIG_FS_CELL_ENDUR=20 # in power

# pmfs with vmap
# pmfs with vmap、tracking and timing
