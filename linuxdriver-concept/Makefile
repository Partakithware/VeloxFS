# VeloxFS v6 Kernel Module Makefile
# Target: Linux 5.15 LTS and later (tested against 5.15, 6.1, 6.6)
#
# Build:    make
# Install:  sudo make install
# Load:     sudo insmod veloxfs_mod.ko
# Unload:   sudo rmmod veloxfs_mod
# Check:    dmesg | grep veloxfs
#
# Format a loop device for testing:
#   dd if=/dev/zero of=test.img bs=1M count=128
#   # Then use the veloxfs userspace tool (veloxfs_format) to write v6 layout
#   sudo losetup /dev/loop0 test.img
#   sudo mount -t veloxfs /dev/loop0 /mnt/velox
#   sudo umount /mnt/velox
#   sudo losetup -d /dev/loop0

obj-m := veloxfs_mod.o

# Kernel build directory (override with KDIR= on the command line)
KDIR  ?= /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)

# Extra warnings during development
ccflags-y := -Wall -Wextra -Wmissing-prototypes

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

.PHONY: all clean install
