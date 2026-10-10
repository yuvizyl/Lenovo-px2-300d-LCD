#!/bin/sh
#
# load.sh - build and dynamically load the wixlcm kernel module
#
# Usage:
#   sh load.sh                      # native build + insmod
#   sudo sh load.sh                 # run as root if you are not already
#
# Environment overrides (cross build):
#   KDIR=/path/to/kernel ARCH=x86 CROSS_COMPILE=i686-linux- sh load.sh
#
# After loading:
#   /dev/lcm    - 128x64 LCD + backlight ioctl
#   /dev/wixbtn - the two LCD buttons (Select/Scroll)
#
set -e

echo "==> Building wixlcm.ko"
if [ -n "$KDIR" ]; then
	make -C "$KDIR" M="$(pwd)" modules
else
	make
fi

# Standard backlight class (CONFIG_BACKLIGHT_CLASS_DEVICE=m on Ubuntu);
# harmless if it is built-in or unavailable. ioctl/sysfs work regardless.
if grep -q '^CONFIG_BACKLIGHT_CLASS_DEVICE=m' \
	"/lib/modules/$(uname -r)/build/.config" 2>/dev/null; then
	echo "==> loading dependency: backlight"
	modprobe backlight 2>/dev/null || true
fi

if lsmod 2>/dev/null | grep -q '^wixlcm '; then
	echo "==> wixlcm already loaded, reloading"
	rmmod wixlcm
fi

echo "==> insmod wixlcm.ko"
insmod wixlcm.ko

# devtmpfs creates /dev/lcm and /dev/wixbtn automatically. On systems
# without devtmpfs, create the nodes manually from /proc/misc.
for name in lcm wixbtn; do
	if [ ! -e "/dev/$name" ]; then
		MINOR=$(sed -n "s/^ *\([0-9][0-9]*\) $name\$/\1/p" /proc/misc)
		if [ -n "$MINOR" ]; then
			echo "==> creating /dev/$name (major 10, minor $MINOR)"
			mknod "/dev/$name" c 10 "$MINOR"
		fi
	fi
	chmod 666 "/dev/$name" 2>/dev/null || true
done

echo "==> device nodes: $(ls /dev/lcm /dev/wixbtn 2>/dev/null)"
echo "==> backlight state: $(cat /sys/class/misc/lcm/backlight 2>/dev/null || echo n/a)"
echo "==> kernel log:"
dmesg 2>/dev/null | tail -n 8 || true
