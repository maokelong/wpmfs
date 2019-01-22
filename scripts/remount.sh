#!/bin/sh
set -e

# switch working dir and load user settings
SCRIPT_DIR=$(cd `dirname $0`; pwd)
cd $SCRIPT_DIR
source config.sh

# umount
sudo umount $CONFIG_FS_DIR
sudo rmmod $CONFIG_FS_NAME

# mount
sudo insmod $CONFIG_FS_NAME.ko measure_timing=0
sleep 1
sudo mount -t $CONFIG_FS_NAME $CONFIG_PMEM_DEV_DIR $CONFIG_FS_DIR
