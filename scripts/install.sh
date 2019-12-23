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
make CPPFLAGS=-DSTOP_TRACKING

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
sudo insmod $CONFIG_NAME_FS.ko measure_timing=0
sleep 1
sudo mount -t $CONFIG_NAME_FS -o init,vmap,dbgmask=$CONFIG_FS_DBGMASK,wlsw=$CONFIG_FS_WL_SWITCH,alloc=$CONFIG_FS_ALLOCATOR \
  $CONFIG_PATH_PMEM_DEV $CONFIG_PATH_FS
USER_NAME=$USER
sudo chown $USER_NAME $CONFIG_PATH_FS
sudo chown $USER_NAME $CONFIG_PATH_PROC
