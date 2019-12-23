#!/bin/sh
set -e

# switch working dir and load user settings
SCRIPT_DIR=$(
  cd $(dirname $0)
  pwd
)
cd $SCRIPT_DIR
source config.sh

# make fs
cd ..
if [[ $CONFIG_FS_ENABLE_TRACKING -ne 0 ]]; then
  make
else
  make CPPFLAGS=-DSTOP_TRACKING
fi

# mkdir, unmount and rmmod
if [[ ! -d $CONFIG_PATH_FS ]]; then
  sudo mkdir -p $CONFIG_PATH_FS
fi

if [[ "$(mountpoint $CONFIG_PATH_FS)" == *"is a"* ]]; then
  sudo umount $CONFIG_PATH_FS
fi

if [[ -n "$(lsmod | grep $CONFIG_NAME_FS)" ]]; then
  sudo rmmod $CONFIG_NAME_FS
fi

# insmod and mount
sudo insmod $CONFIG_NAME_FS.ko measure_timing=$CONFIG_FS_TIMING
sleep 1

# 准备好文件系统参数，最后一个逗号放上去也无所谓
args=""
if [[ $CONFIG_FS_INIT_HARD -ne 0 ]]; then
  args="init,"
fi
if [[ $CONFIG_FS_ENABLE_VMAP -ne 0 ]]; then
  args=$args"vmap,"
fi
args=$args"dbgmask=$CONFIG_FS_DBGMASK,"
args=$args"alloc=$CONFIG_FS_ALLOCATOR,"
args=$args"wlsw=$CONFIG_FS_WL_SWITCH,"

sudo mount -t $CONFIG_NAME_FS -o $args $CONFIG_PATH_PMEM_DEV $CONFIG_PATH_FS

USER_NAME=$USER
sudo chown $USER_NAME $CONFIG_PATH_FS
sudo chown $USER_NAME $CONFIG_PATH_PROC
