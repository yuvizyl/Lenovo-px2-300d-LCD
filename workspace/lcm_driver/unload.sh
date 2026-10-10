#!/bin/sh
#
# unload.sh - remove the wixlcm kernel module (screen is blanked first)
#
# Usage:
#   sh unload.sh
#
set -e

if lsmod 2>/dev/null | grep -q '^wixlcm '; then
	echo "==> rmmod wixlcm"
	rmmod wixlcm
else
	echo "==> wixlcm is not loaded"
fi

dmesg 2>/dev/null | tail -n 3 || true
