#!/bin/sh
set -e

# switch working dir and load user settings
SCRIPT_DIR=$(cd `dirname $0`; pwd)
cd $SCRIPT_DIR
source config.sh

# clean, unmount, rmmod and remove
cd ..
make clean
if [[ "$(mountpoint $CONFIG_FS_DIR)" =~ *"not"* ]]; then
  sudo umount $CONFIG_FS_DIR
fi
if [[ -n "$(lsmod | grep $CONFIG_FS_NAME)" ]]; then
  sudo rmmod $CONFIG_FS_NAME
fi
sudo rm -rf $CONFIG_FS_DIR
