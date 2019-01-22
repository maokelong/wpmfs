#!/bin/sh
set -e

# switch working dir and load user settings
SCRIPT_DIR=$(cd `dirname $0`; pwd)
cd $SCRIPT_DIR
source config.sh

# make fs
cd ..
make

# mkdir or umount
if [ ! -d $CONFIG_FS_DIR ]; then
  USER_NAME=$(whoami)
  sudo mkdir -p $CONFIG_FS_DIR
  sudo chown $USER_NAME $CONFIG_FS_DIR
else
  sudo umount $CONFIG_FS_DIR && rmmod $CONFIG_FS_NAME | :
fi

# mount 
sudo insmod $CONFIG_FS_NAME.ko measure_timing=0
sleep 1
sudo mount -t $CONFIG_FS_NAME -o init $CONFIG_PMEM_DEV_DIR $CONFIG_FS_DIR
