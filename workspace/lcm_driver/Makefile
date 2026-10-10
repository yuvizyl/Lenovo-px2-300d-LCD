# Makefile for the px2-300d front-panel LCM kernel module (out-of-tree)
#
# Native build on the target machine:
#	make
#	sudo insmod wixlcm.ko
#
# Cross build (example for a 32-bit x86 target):
#	make KDIR=/path/to/linux-3.2.40 ARCH=x86 CROSS_COMPILE=i686-linux-
#
# Required kernel config: CONFIG_MODULES=y
# Module parameters (insmod wixlcm.ko sioaddr=0x4E initialize=1 wixdbg=0)

obj-m += wixlcm.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
