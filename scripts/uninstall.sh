#!/bin/sh
set -e

# switch working dir and load user settings
SCRIPT_DIR=$(cd `dirname $0`; pwd)
cd $SCRIPT_DIR
source config.sh

# clean, unmount, rmmod and remove
cd ..
make clean
if [[ "$(mountpoint $CONFIG_PATH_FS)" == *"is a"* ]]; then
  sudo umount $CONFIG_PATH_FS
fi
if [[ -n "$(lsmod | grep $CONFIG_NAME_FS)" ]]; then
  sudo rmmod $CONFIG_NAME_FS
fi
sudo rm -rf $CONFIG_PATH_FS
