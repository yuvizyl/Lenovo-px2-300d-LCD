/* SPDX-License-Identifier: GPL-2.0 */
/*
 * lcm_uapi.h - userspace/kernel shared ABI for the px2-300d front LCM
 *
 * This header is kept binary compatible with the original Wistron
 * LifeLine b2a driver (readfile/b2a/nas.h). Both kernel and userspace
 * (C/C++) code can include it.
 *
 * Screen geometry:
 *   128 x 64 monochrome dots, organized as 8 pages x 128 columns.
 *   Each byte drives 8 vertical dots of one column inside a page
 *   (bit0 = top dot, bit7 = bottom dot).
 */
#ifndef _PX2_WIXLCM_UAPI_H_
#define _PX2_WIXLCM_UAPI_H_

#if defined(__KERNEL__)
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif

#define LCM_IOC_MAGIC		0xDD

/*
 * Main command. Argument is a pointer to struct lcm_info.
 * The action actually performed is selected by lcm_info.ctrl:
 *   WIX_LCM_CMD_PON   - turn display on        (controller 0xAF)
 *   WIX_LCM_CMD_POFF  - turn display off       (controller 0xAE)
 *   WIX_LCM_CMD_RESET - set page/column address and stream display data
 */
#define IOCTL_DISPLAY_COMMAND	_IO(LCM_IOC_MAGIC, 1)

/* Extension: direct display on/off, no argument needed. */
#define WIX_LCM_IOC_PON		_IO(LCM_IOC_MAGIC, 7)
#define WIX_LCM_IOC_POFF	_IO(LCM_IOC_MAGIC, 8)

/*
 * Backlight extension (LCD 背光, AUXFANOUT register in the NCT6776F):
 *   mode 0 = off, 1 = on, 2 = blinking  (identical to vendor semantics:
 *   register values 0xC0 / 0xD5 / 0xFF).
 * Also available at /sys/class/misc/lcm/backlight
 * and (when enabled) /sys/class/backlight/wixlcm/.
 */
#define WIX_LCM_IOC_BL_SET	_IOW(LCM_IOC_MAGIC, 10, int)
#define WIX_LCM_IOC_BL_GET	_IOR(LCM_IOC_MAGIC, 11, int)

#define WIX_LCM_BL_OFF		0
#define WIX_LCM_BL_ON		1
#define WIX_LCM_BL_BLINK	2

/* Values for lcm_info.ctrl (identical to the vendor nas.h definitions). */
#define WIX_LCM_CMD_PON		7
#define WIX_LCM_CMD_POFF	8
#define WIX_LCM_CMD_RESET	9

#define LCM_PAGES		8	/* pages 0..7, each 8 dots high      */
#define LCM_COLUMNS		128	/* columns 0..127                    */
#define LCM_DATA_MAX		128	/* max bytes per write (one page)    */

struct lcm_info {
	int ctrl;				 /* WIX_LCM_CMD_*                   */
	int page;				 /* page  number 0..7               */
	int column;			 /* start column 0..127             */
	int size;				 /* valid bytes in data[], <= 128   */
					 /* column + size must be <= 128    */
	char data[LCM_DATA_MAX];	 /* vertical dot data               */
};

/* ----------------------------------------------------------------------- */
/* Front panel buttons  (/dev/wixbtn)                                       */
/*                                                                         */
/* The two LCD buttons are routed by the BIOS hardware to ACPI GPE18/GPE19  */
/* and report one event per key press, same as the vendor /dev/wixbtn ABI:  */
/* magic 'c', ioctl 150 non-blocking / 151 blocking, argument is a pointer  */
/* to int with one of the WIX_BTN_* codes. Plain read(2) on /dev/wixbtn    */
/* also returns 4-byte little-endian ints.                                  */
/* ----------------------------------------------------------------------- */

#define WIX_NO_BTN		0
#define WIX_BTN_POWER		1	/* (not reported by this driver)    */
#define WIX_BTN_RESET_BRIEFLY	2	/* (not reported by this driver)    */
#define WIX_BTN_RESET_LONG	3	/* (not reported by this driver)    */
#define WIX_BTN_LCM_SELECT	4	/* LCD 旁边的"选择/确认"键          */
#define WIX_BTN_LCM_SCROLL	5	/* LCD 旁边的"滚动/翻页"键          */

#define READ_BUTTON_NONBLOCKING	_IOWR('c', 150, int)
#define READ_BUTTON_BLOCKING	_IOWR('c', 151, int)

#endif /* _PX2_WIXLCM_UAPI_H_ */
