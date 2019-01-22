#
# Makefile for the linux wapmfs-filesystem routines.
#

obj-m += wapmfs.o

wapmfs-y := bbuild.o balloc.o dir.o file.o inode.o namei.o super.o symlink.o ioctl.o pmfs_stats.o journal.o xip.o wprotect.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=`pwd`

clean:
	make -C /lib/modules/$(shell uname -r)/build M=`pwd` clean
