// SPDX-License-Identifier: GPL-2.0
/*
 * wixlcm.c - LenovoEMC px2-300d front-panel driver
 *
 * A standalone, dynamically loadable (out-of-tree kernel module) rewrite
 * of the Wistron "wixlcm"/"sci"/"led" code shipped inside the LifeLine
 * FossKit b2a kernel patch (drivers/b2a/{lcm,sci,gpiosio}.c).
 *
 * This module drives three pieces of the px2-300d front panel:
 *
 *   1. 128x64 monochrome LCM  -> misc device /dev/lcm
 *      The panel hangs off the Nuvoton NCT6776F Super-I/O GPIO pins;
 *      the driver bit-bangs an 8080 style parallel bus through the SIO
 *      config index/data ports (default 0x4E/0x4F, chip id 0xC3).
 *
 *      Control pins (group.bit, 0x0Ggx => group G, bit x):
 *        A0  GP01 (0x0001)   0 = command, 1 = data
 *        CS# GP02 (0x0002)
 *        E   GP03 (0x0003)   latch clock
 *        RS  GP04 (0x0004)
 *        R/W#GP05 (0x0005)   0 = write
 *        D0  GP06 (0x0006)   D1 GP07 (0x0007)
 *        D2  GP46 (0x0406)   D3 GP87 (0x0807)   D4 GP86 (0x0806)
 *        D5  GP84 (0x0804)   D6 GP81 (0x0801)   D7 GP80 (0x0800)
 *
 *   2. LCD backlight -> ioctl on /dev/lcm, sysfs
 *      /sys/class/misc/lcm/backlight (0=off/1=on/2=blink) and, when the
 *      kernel provides it, a standard Linux backlight device
 *      /sys/class/backlight/wixlcm/.
 *      Hardware: the LED hangs off an NCT6776F AUXFANOUT PWM pin. Init
 *      must first select the AUXFAN function: global CR 0x1B |= 0x40 and
 *      LDN 0x0B CR 0xF8 |= 0x07 (verbatim from gpiosio.c probe), otherwise
 *      the pin stays as GPIO and the duty value can only dim, not switch
 *      the light off. Then drive it through HWM indirect ports
 *      (base+5 index / base+6 data), AUXFANOUT bank 0x4E reg 0x0309:
 *      0xC0 = off, 0xD5 = on, 0xFF = blink.
 *
 *   3. Two LCD buttons (Select / Scroll) -> misc device /dev/wixbtn
 *      They sit on Intel ICH8/PCH GPIO2/GPIO3. The original b2a/sci.c
 *      detected edges via ACPI GPE18/GPE19 + the GPIO INV polarity trick,
 *      but on modern kernels a level-triggered GPE that does not deassert
 *      quickly enough triggers the ACPICA GPE-storm protection, which
 *      disables the GPE permanently (first key works, later keys dead until
 *      module reload). This driver therefore polls the GPIO level register
 *      with a debouncing kernel timer instead - it cannot storm and survives
 *      userspace restarts. One WIX_BTN_LCM_SELECT(4) / WIX_BTN_LCM_SCROLL(5)
 *      event is reported per key press. Usable with blocking/non-blocking
 *      ioctl, plain read(2) (4-byte little-endian ints), and poll(2).
 *
 * Build/load:
 *   make
 *   insmod wixlcm.ko            (devtmpfs creates /dev/lcm,/dev/wixbtn)
 *
 * Copyright (c) 2008-2013 Wistron Corp. (original b2a drivers)
 * Standalone module adaptation for px2-300d.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/ioport.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/pci.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/version.h>
#include <linux/acpi.h>

/* timer API renamed in kernel 6.15: del_timer_sync() -> timer_delete_sync() */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
#define wix_timer_delete_sync(t)	timer_delete_sync(t)
#else
#define wix_timer_delete_sync(t)	del_timer_sync(t)
#endif
#if IS_ENABLED(CONFIG_BACKLIGHT_CLASS_DEVICE)
#include <linux/fb.h>
#include <linux/backlight.h>
#endif

#include "lcm_uapi.h"

#define WIXLCM_VERSION		"1.1"
#define LCM_DEVICE_NAME		"lcm"
#define BTN_DEVICE_NAME		"wixbtn"
#define SIO_ADDR_DEFAULT	0x4E

/* ---- LCM pin definitions (0x0Ggx: G = group, x = bit) ---- */
#define LCM_BUS_A0		0x0001	/* GP01 */
#define LCM_BUS_CS		0x0002	/* GP02 */
#define LCM_BUS_E		0x0003	/* GP03 */
#define LCM_BUS_RS		0x0004	/* GP04 */
#define LCM_BUS_RW		0x0005	/* GP05 */
#define D0			0x0006	/* GP06 */
#define D1			0x0007	/* GP07 */
#define D2			0x0406	/* GP46 */
#define D3			0x0807	/* GP87 */
#define D4			0x0806	/* GP86 */
#define D5			0x0804	/* GP84 */
#define D6			0x0801	/* GP81 */
#define D7			0x0800	/* GP80 */

#define LCM_DATA_BITS		8
#define PIN_LOW			0
#define PIN_HIGH		1

#define A0_DAT			0x01
#define A0_CMD			0x00

#define LCM_DATABUS_OUTPUT	0x00
#define LCM_DATABUS_INPUT	0x01

#define LCM_OFF_CMD		0xAE
#define LCM_ON_CMD		0xAF

#define GPP_GROUP(PIN_DEF)	((PIN_DEF) >> 8)
#define GPP_BIT(PIN_DEF)	((PIN_DEF) & 0x00FF)

#define INIT_PACKAGE_SIZE	13

/* ---- backlight: NCT6776F HWM indirect register access ---- */
#define HWM_LDN			0x0B
#define AUXFANOUT_RD		0x0011
#define AUXFANOUT_WR		0x0309
#define BANK_SEL		0x004E
static const unsigned char bl_regval[3] = { 0xC0, 0xD5, 0xFF };

/* ---- buttons: ICH8/PCH GPIO, polled by a debouncing kernel timer ---- */
#define LPC_VENDORID		0x8086
#define LPC_DEVICEID		0x2918	/* Intel ICH8 LPC */
#define LPC_OFFSET_GPIOBASE	0x48

#define PCH_GPIO_USE_SEL	0x00
#define PCH_GPIO_IO_SEL		0x04
#define PCH_GPIO_LVL		0x0C
#define PCH_GPIO_INV		0x2C

/* LCD buttons: PCH GPIO index */
#define BTN_SEL_GPIO_IDX	2
#define BTN_SCR_GPIO_IDX	3

/* Polling/debounce timing */
#define BTN_POLL_MS		20	/* timer period */
#define BTN_DEBOUNCE_MS		60	/* level must be stable this long */

/* Module parameters */
static unsigned short sioaddr = SIO_ADDR_DEFAULT;
module_param(sioaddr, ushort, 0444);
MODULE_PARM_DESC(sioaddr, "Super-I/O config index port (default 0x4E)");

static bool initialize = true;
module_param(initialize, bool, 0644);
MODULE_PARM_DESC(initialize, "initialize and light up the panel on module load (default Y)");

static int initial_backlight = WIX_LCM_BL_ON;
module_param(initial_backlight, int, 0644);
MODULE_PARM_DESC(initial_backlight, "backlight state after load: 0=off 1=on 2=blink (default 1)");

static bool enable_buttons = true;
module_param(enable_buttons, bool, 0644);
MODULE_PARM_DESC(enable_buttons, "register the two LCD buttons on /dev/wixbtn (default Y)");

static int wixdbg;
module_param(wixdbg, int, 0644);
MODULE_PARM_DESC(wixdbg, "verbose GPIO/register debug traces (default 0)");

#define lcm_dbg(fmt, ...)					\
	do {							\
		if (wixdbg)					\
			pr_info("wixlcm: " fmt, ##__VA_ARGS__);	\
	} while (0)

static DEFINE_MUTEX(sio_lock);	/* serialize all Super-I/O accesses */
static DEFINE_MUTEX(lcm_lock);	/* serialize panel command sequences */

static bool region_held;
static int data_bus_mode = -1;	/* -1 = unknown */
static bool lcm_in_use;		/* exclusive open flag */

static const int data_pin_mapping[LCM_DATA_BITS] = {
	D0, D1, D2, D3, D4, D5, D6, D7
};

/* ST7565/UC160x compatible power-up sequence (identical to vendor driver) */
static const unsigned char initial_lcm_pkg[INIT_PACKAGE_SIZE] = {
	0xAE,	/* display off                 */
	0xA2,	/* LCD bias 1/9                */
	0xA0,	/* ADC normal (segment dir)    */
	0xC8,	/* COM reverse (common dir)    */
	0xA6,	/* normal display              */
	0x40,	/* start line 0                */
	0x22,	/* resistor ratio              */
	0x81,	/* electronic volume set       */
	0x3F,	/* volume = 0x3F               */
	0xF8,	/* booster ratio command        */
	0x00,	/* booster = 4x                 */
	0xA4,	/* display RAM content         */
	0x2C,	/* power control: regulator on  */
};

/* ======================================================================= */
/* Super-I/O index/data port helpers                                        */
/* ======================================================================= */

static inline void sio_enter_cfg(void)
{
	outb_p(0x87, sioaddr);
	outb_p(0x87, sioaddr);
}

static inline void sio_exit_cfg(void)
{
	outb_p(0xAA, sioaddr);
}

static inline void sio_select_ldn(unsigned char ldn)
{
	outb_p(0x07, sioaddr);
	outb_p(ldn, sioaddr + 1);
}

/*
 * Resolve GPIO group id (0x00/0x02/0x04/0x08) to its Super-I/O logical
 * device number and GPIO data register.
 */
static int gpp_resolve(unsigned char grp_id, unsigned char *ldn,
		       unsigned char *data_reg)
{
	switch (grp_id) {
	case 0x00:
		*ldn = 0x08;
		*data_reg = 0xE1;
		return 0;
	case 0x02:
		*ldn = 0x09;
		*data_reg = 0xE1;
		return 0;
	case 0x04:
		*ldn = 0x09;
		*data_reg = 0xF1;
		return 0;
	case 0x08:
		*ldn = 0x07;
		*data_reg = 0xE5;
		return 0;
	default:
		return -EINVAL;
	}
}

static void nct_gpp_value_set(unsigned char grp_id, unsigned char gp_offset,
			      unsigned char value)
{
	unsigned char ldn = 0, data_reg = 0, org_val, data;

	if (gpp_resolve(grp_id, &ldn, &data_reg) != 0)
		return;

	mutex_lock(&sio_lock);
	sio_enter_cfg();
	sio_select_ldn(ldn);

	outb_p(data_reg, sioaddr);
	org_val = inb(sioaddr + 1);

	if (value == 1)
		data = org_val | (1 << gp_offset);
	else
		data = org_val & (~(1 << gp_offset) & 0xFF);

	outb_p(data_reg, sioaddr);
	outb_p(data, sioaddr + 1);

	sio_exit_cfg();
	mutex_unlock(&sio_lock);

	lcm_dbg("grp=%d bit=%d value=%d reg=0x%02X->0x%02X\n",
		grp_id, gp_offset, value, data_reg, data);
}

/*
 * Switch the 8 data bus pins (spread over GPIO groups 0/4/8) between
 * output and input direction. Register masks are copied verbatim from
 * the proven vendor driver.
 */
static void set_databus_mode(int mode)
{
	int val;

	if (data_bus_mode == mode)
		return;

	mutex_lock(&sio_lock);
	sio_enter_cfg();

	switch (mode) {
	case LCM_DATABUS_OUTPUT:
		/* D0,D1 (GP06,GP07) as output */
		sio_select_ldn(0x08);
		outb_p(0xE0, sioaddr);
		val = inb(sioaddr + 1);
		outb_p(0xE0, sioaddr);
		outb_p(val & 0xFC, sioaddr + 1);
		/* D2 (GP46) as output */
		sio_select_ldn(0x09);
		outb_p(0xF0, sioaddr);
		val = inb(sioaddr + 1);
		outb_p(0xF0, sioaddr);
		outb_p(val & 0xBF, sioaddr + 1);
		/* D3..D7 (GP87,GP86,GP84,GP81,GP80) as output */
		sio_select_ldn(0x07);
		outb_p(0xE4, sioaddr);
		val = inb(sioaddr + 1);
		outb_p(0xE4, sioaddr);
		outb_p(val & 0xC2, sioaddr + 1);
		break;
	case LCM_DATABUS_INPUT:
		/* D0,D1 (GP06,GP07) as input */
		sio_select_ldn(0x08);
		outb_p(0xE0, sioaddr);
		val = inb(sioaddr + 1);
		outb_p(0xE0, sioaddr);
		outb_p(val | 0x03, sioaddr + 1);
		/* D2 (GP46) as input */
		sio_select_ldn(0x09);
		outb_p(0xF0, sioaddr);
		val = inb(sioaddr + 1);
		outb_p(0xF0, sioaddr);
		outb_p(val | 0x40, sioaddr + 1);
		/* D3..D7 (GP87,GP86,GP84,GP81,GP80) as input */
		sio_select_ldn(0x07);
		outb_p(0xE4, sioaddr);
		val = inb(sioaddr + 1);
		outb_p(0xE4, sioaddr);
		outb_p(val | 0xD3, sioaddr + 1);
		break;
	default:
		sio_exit_cfg();
		mutex_unlock(&sio_lock);
		return;
	}

	sio_exit_cfg();
	mutex_unlock(&sio_lock);
	data_bus_mode = mode;
}

static void set_databus_data(int data)
{
	int i;
	unsigned char bit_data;

	lcm_dbg("write data=0x%02X to LCM\n", data);

	for (i = 0; i < LCM_DATA_BITS; i++) {
		bit_data = data & 0x01;
		nct_gpp_value_set(GPP_GROUP(data_pin_mapping[i]),
				  GPP_BIT(data_pin_mapping[i]), bit_data);
		data >>= 1;
	}
}

static void set_ctrlpin_data(int ctrl_pin, unsigned char data)
{
	switch (ctrl_pin) {
	case LCM_BUS_A0:
	case LCM_BUS_CS:
	case LCM_BUS_E:
	case LCM_BUS_RS:
	case LCM_BUS_RW:
		nct_gpp_value_set(GPP_GROUP(ctrl_pin),
				  GPP_BIT(ctrl_pin), data);
		break;
	default:
		break;
	}
}

/* Write one command (A0type == A0_CMD) or data byte (A0type == A0_DAT). */
static void write_lcm(int a0_type, unsigned char value)
{
	set_databus_mode(LCM_DATABUS_OUTPUT);

	/* latch clock low, place byte on data bus */
	set_ctrlpin_data(LCM_BUS_E, PIN_LOW);
	set_databus_data(value);

	/* A0: command/data */
	if (a0_type == A0_CMD)
		set_ctrlpin_data(LCM_BUS_A0, PIN_LOW);
	else
		set_ctrlpin_data(LCM_BUS_A0, PIN_HIGH);

	/* write direction, assert chip select */
	set_ctrlpin_data(LCM_BUS_RW, PIN_LOW);
	set_ctrlpin_data(LCM_BUS_CS, PIN_LOW);

	/* rising edge latches the byte */
	set_ctrlpin_data(LCM_BUS_E, PIN_HIGH);
	udelay(100);
	set_ctrlpin_data(LCM_BUS_E, PIN_LOW);
	set_ctrlpin_data(LCM_BUS_CS, PIN_HIGH);
	udelay(100);
}

static void initialize_lcm(void)
{
	int i;

	for (i = 0; i < INIT_PACKAGE_SIZE; i++)
		write_lcm(A0_CMD, initial_lcm_pkg[i]);

	mdelay(200);
	write_lcm(A0_CMD, 0x2E);	/* power follower on       */
	mdelay(200);
	write_lcm(A0_CMD, 0x2F);	/* internal booster on    */
	mdelay(400);
	write_lcm(A0_CMD, LCM_ON_CMD);	/* display on             */
}

/* ======================================================================= */
/* Backlight  (NCT6776F AUXFANOUT via HWM logical device 0x0B)              */
/* ======================================================================= */

static unsigned int hwm_idx_port;	/* LDN 0x0B base + 5 */
static unsigned int hwm_data_port;	/* LDN 0x0B base + 6 */
static int backlight_state = WIX_LCM_BL_OFF;
static DEFINE_MUTEX(bl_lock);
#if IS_ENABLED(CONFIG_BACKLIGHT_CLASS_DEVICE)
static struct backlight_device *wix_bl_dev;
#endif

static int backlight_hw_probe(void)
{
	unsigned char hi, lo, val;
	unsigned int base;

	mutex_lock(&sio_lock);
	sio_enter_cfg();

	/*
	 * Global CR 0x1B: enable GP44/GP45 multi-function pins
	 * (verbatim from b2a/gpiosio.c siogphost_probe()).
	 */
	outb_p(0x1B, sioaddr);
	val = inb_p(sioaddr + 1);
	outb_p(0x1B, sioaddr);
	outb_p(val | 0x40, sioaddr + 1);

	/*
	 * LDN 0x0B CR 0xF8: AUXFANOUT0/1/2 pin function select.
	 * Each of bits 2:0 switches one pin: 0 = plain GPIO,
	 * 1 = AUXFAN (PWM) output. The LCD backlight hangs off one of
	 * these pins; without selecting the AUXFAN function the duty
	 * register cannot drive it fully off (observed: writing 0xC0
	 * only dims the backlight). Enable all three, as the vendor
	 * driver does (it wrote 0x07 outright; RMW keeps upper bits).
	 */
	sio_select_ldn(HWM_LDN);
	outb_p(0xF8, sioaddr);
	val = inb_p(sioaddr + 1);
	outb_p(0xF8, sioaddr);
	outb_p(val | 0x07, sioaddr + 1);

	outb_p(0x60, sioaddr);
	hi = inb_p(sioaddr + 1);
	outb_p(0x61, sioaddr);
	lo = inb_p(sioaddr + 1);
	sio_exit_cfg();
	mutex_unlock(&sio_lock);

	base = ((unsigned int)hi << 8) | lo;
	if (base == 0 || base < 0x100) {
		pr_warn("wixlcm: no HWM LPC base for LDN 0x%02X, backlight disabled\n",
			HWM_LDN);
		return -ENXIO;
	}

	hwm_idx_port = base + 0x05;
	hwm_data_port = base + 0x06;

	lcm_dbg("backlight pin mux: CR1B|=0x40, LDN0x%02X CRF8|=0x07, "
		"HWM base 0x%04X\n", HWM_LDN, base);
	return 0;
}

/* Read AUXFANOUT (0x0011), map high nibble C/D/F to state 0/1/2. */
static int backlight_hw_get(void)
{
	unsigned char v;

	if (!hwm_idx_port)
		return -ENXIO;

	mutex_lock(&sio_lock);
	outb(BANK_SEL & 0xFF, hwm_idx_port);
	outb((AUXFANOUT_RD >> 8) & 0xFF, hwm_data_port);
	outb(AUXFANOUT_RD & 0xFF, hwm_idx_port);
	v = inb(hwm_data_port);
	mutex_unlock(&sio_lock);

	switch (v & 0xF0) {
	case 0xC0:
		return WIX_LCM_BL_OFF;
	case 0xD0:
		return WIX_LCM_BL_ON;
	case 0xF0:
		return WIX_LCM_BL_BLINK;
	default:
		return -EIO;
	}
}

/* Write AUXFANOUT (0x0309): 0xC0 off / 0xD5 on / 0xFF blink. */
static int backlight_hw_set(int state)
{
	if (!hwm_idx_port)
		return -ENXIO;
	if (state < WIX_LCM_BL_OFF || state > WIX_LCM_BL_BLINK)
		return -EINVAL;

	mutex_lock(&sio_lock);
	outb(BANK_SEL & 0xFF, hwm_idx_port);
	outb((AUXFANOUT_WR >> 8) & 0xFF, hwm_data_port);
	outb(AUXFANOUT_WR & 0xFF, hwm_idx_port);
	outb(bl_regval[state], hwm_data_port);

	if (wixdbg) {
		unsigned char rd;

		/* read back bank0/0x11 (AUXFANOUT_RD) to confirm the write */
		outb(BANK_SEL & 0xFF, hwm_idx_port);
		outb((AUXFANOUT_RD >> 8) & 0xFF, hwm_data_port);
		outb(AUXFANOUT_RD & 0xFF, hwm_idx_port);
		rd = inb(hwm_data_port);
		pr_info("wixlcm: backlight set 0x%02X, readback 0x%02X\n",
			bl_regval[state], rd);
	}
	mutex_unlock(&sio_lock);

	lcm_dbg("backlight -> %d (reg 0x%02X)\n", state, bl_regval[state]);
	return 0;
}

static int backlight_set_state(int state)
{
	int ret;

	if (state < WIX_LCM_BL_OFF || state > WIX_LCM_BL_BLINK)
		return -EINVAL;

	mutex_lock(&bl_lock);
	ret = backlight_hw_set(state);
	if (ret == 0)
		backlight_state = state;
	mutex_unlock(&bl_lock);
	return ret;
}

/* ======================================================================= */
/* Hardware detection / LCM pin mux setup                                   */
/* ======================================================================= */

static int lcm_hw_detect_and_config(void)
{
	int val;

	mutex_lock(&sio_lock);
	sio_enter_cfg();

	/* chip id: NCT6776F should answer 0xC3 */
	outb_p(0x20, sioaddr);
	val = inb(sioaddr + 1);
	if (val != 0xC3) {
		sio_exit_cfg();
		mutex_unlock(&sio_lock);
		pr_err("wixlcm: Super-I/O at 0x%X reports id 0x%02X, expected 0xC3\n",
		       sioaddr, val);
		return -ENXIO;
	}

	/* global register: clear bit 6 */
	outb_p(0x24, sioaddr);
	val = inb(sioaddr + 1);
	outb_p(0x24, sioaddr);
	outb_p(val & 0xBF, sioaddr + 1);

	/*
	 * LDN 0x08 (GPIO group 0) pin direction register 0xE0:
	 * configure A0/CS/E/RS/RW (GP01..GP05) as outputs.
	 */
	sio_select_ldn(0x08);
	outb_p(0xE0, sioaddr);
	val = inb(sioaddr + 1);
	outb_p(0xE0, sioaddr);
	outb_p(val & 0x83, sioaddr + 1);
	outb_p(0xE0, sioaddr);
	val = inb(sioaddr + 1);

	sio_exit_cfg();
	mutex_unlock(&sio_lock);

	/* E idle low; force data bus direction to be re-programmed */
	set_ctrlpin_data(LCM_BUS_E, PIN_LOW);
	data_bus_mode = -1;

	return 0;
}

/* ======================================================================= */
/* /dev/lcm character device                                                */
/* ======================================================================= */

static long lcmdev_ioctl(struct file *filp, unsigned int cmd,
			 unsigned long arg)
{
	struct lcm_info info;
	unsigned char tmp;
	int ret = 0;
	int i;
	int bl;

	switch (cmd) {
	case WIX_LCM_IOC_PON:
		mutex_lock(&lcm_lock);
		write_lcm(A0_CMD, LCM_ON_CMD);
		mutex_unlock(&lcm_lock);
		return 0;

	case WIX_LCM_IOC_POFF:
		mutex_lock(&lcm_lock);
		write_lcm(A0_CMD, LCM_OFF_CMD);
		mutex_unlock(&lcm_lock);
		return 0;

	case WIX_LCM_IOC_BL_SET:
		if (copy_from_user(&bl, (int __user *)arg, sizeof(bl)))
			return -EFAULT;
		ret = backlight_set_state(bl);
		if (ret)
			return ret;
#if IS_ENABLED(CONFIG_BACKLIGHT_CLASS_DEVICE)
		if (wix_bl_dev)
			wix_bl_dev->props.brightness = backlight_state;
#endif
		return 0;

	case WIX_LCM_IOC_BL_GET: {
		int hw = backlight_hw_get();	/* 读硬件实际状态 */

		mutex_lock(&bl_lock);
		bl = (hw >= 0) ? hw : backlight_state;
		mutex_unlock(&bl_lock);
		if (copy_to_user((int __user *)arg, &bl, sizeof(bl)))
			return -EFAULT;
		return 0;
	}

	case IOCTL_DISPLAY_COMMAND:
		if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
			return -EFAULT;

		lcm_dbg("ctrl=%d page=%d col=%d size=%d\n",
			info.ctrl, info.page, info.column, info.size);

		mutex_lock(&lcm_lock);
		switch (info.ctrl) {
		case WIX_LCM_CMD_PON:
			write_lcm(A0_CMD, LCM_ON_CMD);
			break;
		case WIX_LCM_CMD_POFF:
			write_lcm(A0_CMD, LCM_OFF_CMD);
			break;
		case WIX_LCM_CMD_RESET:
			if (info.page < 0 || info.page >= LCM_PAGES ||
			    info.column < 0 || info.column >= LCM_COLUMNS ||
			    info.size < 0 || info.size > LCM_DATA_MAX ||
			    info.column + info.size > LCM_COLUMNS) {
				ret = -EINVAL;
				break;
			}
			/* page address */
			tmp = (unsigned char)info.page;
			write_lcm(A0_CMD, tmp | 0xB0);
			/* column address high / low nibble */
			tmp = (unsigned char)info.column;
			write_lcm(A0_CMD, ((tmp >> 4) & 0x0F) | 0x10);
			write_lcm(A0_CMD, tmp & 0x0F);
			/* dot data */
			for (i = 0; i < info.size; i++)
				write_lcm(A0_DAT, (unsigned char)info.data[i]);
			break;
		default:
			ret = -EINVAL;
			break;
		}
		mutex_unlock(&lcm_lock);
		return ret;

	default:
		return -ENOTTY;
	}
}

static int lcmdev_open(struct inode *inode, struct file *file)
{
	mutex_lock(&lcm_lock);
	if (lcm_in_use) {
		mutex_unlock(&lcm_lock);
		pr_warn("wixlcm: /dev/lcm is already opened exclusively\n");
		return -EBUSY;
	}
	lcm_in_use = true;
	mutex_unlock(&lcm_lock);
	return 0;
}

static int lcmdev_release(struct inode *inode, struct file *file)
{
	mutex_lock(&lcm_lock);
	lcm_in_use = false;
	mutex_unlock(&lcm_lock);
	return 0;
}

/* /sys/class/misc/lcm/backlight : 0=off 1=on 2=blink */
static ssize_t backlight_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	int val;

	mutex_lock(&bl_lock);
	val = backlight_state;
	mutex_unlock(&bl_lock);
	return sprintf(buf, "%d\n", val);
}

static ssize_t backlight_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;
	ret = backlight_set_state(val);
	if (ret)
		return ret;
#if IS_ENABLED(CONFIG_BACKLIGHT_CLASS_DEVICE)
	if (wix_bl_dev)
		wix_bl_dev->props.brightness = backlight_state;
#endif
	return count;
}

static DEVICE_ATTR_RW(backlight);

static const struct file_operations lcmdev_fops = {
	.owner		= THIS_MODULE,
	.open		= lcmdev_open,
	.release	= lcmdev_release,
	.unlocked_ioctl	= lcmdev_ioctl,
	.compat_ioctl	= lcmdev_ioctl,
};

static struct miscdevice lcmdev = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= LCM_DEVICE_NAME,
	.fops		= &lcmdev_fops,
};

#if IS_ENABLED(CONFIG_BACKLIGHT_CLASS_DEVICE)
static int wix_backlight_update(struct backlight_device *bd)
{
	int brightness = bd->props.brightness;

	if (brightness < WIX_LCM_BL_OFF)
		brightness = WIX_LCM_BL_OFF;
	if (brightness > WIX_LCM_BL_BLINK)
		brightness = WIX_LCM_BL_BLINK;
	return backlight_set_state(brightness);
}

static const struct backlight_ops wix_backlight_ops = {
	.options	= BL_CORE_SUSPENDRESUME,
	.update_status	= wix_backlight_update,
};
#endif

/* ======================================================================= */
/* /dev/wixbtn - the two LCD buttons (Select / Scroll), polled PCH GPIO    */
/* ======================================================================= */

#if IS_ENABLED(CONFIG_PCI)

struct btn_event_node {
	struct list_head	list;
	int			code;	/* WIX_BTN_LCM_* */
};

struct wix_button {
	int			gpio_idx;	/* PCH GPIO index 2/3 */
	int			code;		/* reported key code 4/5 */
	const char		*name;
	/* debounce state, all updated only from the poll timer */
	int			idle_level;	/* level while not pressed (sampled at init) */
	int			last_raw;	/* last raw sample */
	int			stable;		/* last debounced (stable) level */
	unsigned long		stable_jiffies;/* when current run of equal samples began */
};

static LIST_HEAD(btn_events);
static DEFINE_SPINLOCK(btn_event_lock);
static DECLARE_WAIT_QUEUE_HEAD(btn_wait);

static unsigned int pch_gpio_base;
static struct timer_list btn_timer;
static bool btn_timer_on;

static struct wix_button button_sel = {
	.gpio_idx = BTN_SEL_GPIO_IDX,
	.code = WIX_BTN_LCM_SELECT, .name = "LCM Select"
};
static struct wix_button button_scr = {
	.gpio_idx = BTN_SCR_GPIO_IDX,
	.code = WIX_BTN_LCM_SCROLL, .name = "LCM Scroll"
};
static struct wix_button *const buttons[2] = { &button_sel, &button_scr };

/* Callable from timer softirq; only I/O ports + GFP_ATOMIC allocs. */
static void btn_event_enqueue(int code)
{
	struct btn_event_node *node;
	unsigned long flags;

	node = kmalloc(sizeof(*node), GFP_ATOMIC);
	if (!node) {
		pr_err_ratelimited("wixlcm: dropped button event %d (no mem)\n",
				   code);
		return;
	}
	node->code = code;

	spin_lock_irqsave(&btn_event_lock, flags);
	list_add_tail(&node->list, &btn_events);
	spin_unlock_irqrestore(&btn_event_lock, flags);

	wake_up_interruptible(&btn_wait);
}

/*
 * Poll timer: read the two GPIO level bits, software-debounce them and
 * report one event per key press (the transition away from the idle level).
 * Polling replaces the GPE/INV scheme entirely - no level-triggered GPE
 * means no GPE storm, and this keeps working regardless of userspace.
 */
static void btn_poll_timer(struct timer_list *unused)
{
	u32 lvl;
	int i;

	(void)unused;
	if (!pch_gpio_base)
		goto rearm;

	lvl = inl(pch_gpio_base + PCH_GPIO_LVL);

	for (i = 0; i < ARRAY_SIZE(buttons); i++) {
		struct wix_button *b = buttons[i];
		int raw = (lvl >> b->gpio_idx) & 0x01;
		unsigned long now = jiffies;

		if (raw != b->last_raw) {
			b->last_raw = raw;
			b->stable_jiffies = now;
			/* not stable yet */
			continue;
		}

		if (raw == b->stable)
			continue;	/* already at this stable level */

		if (time_before(now, b->stable_jiffies +
			       msecs_to_jiffies(BTN_DEBOUNCE_MS)))
			continue;	/* wait until it stays stable long enough */

		/* accept the new stable level */
		b->stable = raw;
		if (raw != b->idle_level) {
			btn_event_enqueue(b->code);
			lcm_dbg("%s button pressed (polled GPIO%d)\n",
				b->name, b->gpio_idx);
		}
	}

rearm:
	if (btn_timer_on)
		mod_timer(&btn_timer,
			  jiffies + msecs_to_jiffies(BTN_POLL_MS));
}

static int btn_events_pending(void)
{
	int pending;
	unsigned long flags;

	spin_lock_irqsave(&btn_event_lock, flags);
	pending = !list_empty(&btn_events);
	spin_unlock_irqrestore(&btn_event_lock, flags);
	return pending;
}

/* Pop one event; returns WIX_NO_BTN (0) when the queue is empty. */
static int btn_event_pop(void)
{
	struct btn_event_node *node;
	int code = WIX_NO_BTN;
	unsigned long flags;

	spin_lock_irqsave(&btn_event_lock, flags);
	if (!list_empty(&btn_events)) {
		node = list_first_entry(&btn_events, struct btn_event_node, list);
		list_del(&node->list);
		code = node->code;
		kfree(node);
	}
	spin_unlock_irqrestore(&btn_event_lock, flags);
	return code;
}

static int wixbtn_open(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t wixbtn_read(struct file *file, char __user *buf, size_t count,
			   loff_t *ppos)
{
	int code;
	ssize_t written = 0;

	if (count < sizeof(int))
		return -EINVAL;

	if (file->f_flags & O_NONBLOCK) {
		if (!btn_events_pending())
			return -EAGAIN;
	} else {
		int ret = wait_event_interruptible(btn_wait,
						    btn_events_pending());
		if (ret)
			return ret;
	}

	while (written + sizeof(int) <= count) {
		code = btn_event_pop();
		if (code == WIX_NO_BTN)
			break;
		if (copy_to_user(buf + written, &code, sizeof(int)))
			return -EFAULT;
		written += sizeof(int);
	}

	return written;
}

static __poll_t wixbtn_poll(struct file *file, poll_table *wait)
{
	__poll_t mask = 0;

	poll_wait(file, &btn_wait, wait);
	if (btn_events_pending())
		mask |= EPOLLIN | EPOLLRDNORM;
	return mask;
}

static long wixbtn_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int code;
	int blocking;

	switch (cmd) {
	case READ_BUTTON_BLOCKING:
		blocking = 1;
		break;
	case READ_BUTTON_NONBLOCKING:
		blocking = 0;
		break;
	default:
		return -ENOTTY;
	}

	if (blocking) {
		int ret = wait_event_interruptible(btn_wait,
						    btn_events_pending());
		if (ret)
			return ret;
		code = btn_event_pop();
	} else {
		code = btn_event_pop();	/* 0 = WIX_NO_BTN when idle */
	}

	if (copy_to_user((int __user *)arg, &code, sizeof(code)))
		return -EFAULT;
	return 0;
}

static const struct file_operations wixbtn_fops = {
	.owner		= THIS_MODULE,
	.open		= wixbtn_open,
	.read		= wixbtn_read,
	.unlocked_ioctl	= wixbtn_ioctl,
	.compat_ioctl	= wixbtn_ioctl,
	.poll		= wixbtn_poll,
	// .llseek		= no_llseek,
};

static struct miscdevice wixbtndev = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= BTN_DEVICE_NAME,
	.fops		= &wixbtn_fops,
};

static bool wixbtn_registered;

static void wixbtn_cleanup_events(void)
{
	struct btn_event_node *node, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&btn_event_lock, flags);
	list_for_each_entry_safe(node, tmp, &btn_events, list) {
		list_del(&node->list);
		kfree(node);
	}
	spin_unlock_irqrestore(&btn_event_lock, flags);
}

/*
 * Configure the two LCD buttons as plain PCH GPIO inputs and start the
 * debouncing poll timer. No ACPI GPE is touched, which removes the
 * GPE-storm failure mode of the old edge detection.
 */
static int wix_buttons_init(void)
{
	struct pci_dev *pcidev;
	u32 gpiobase = 0;
	u32 pin_mask = (1u << BTN_SEL_GPIO_IDX) | (1u << BTN_SCR_GPIO_IDX);
	u32 lvl;
	int ret, i;

	pcidev = pci_get_device(LPC_VENDORID, LPC_DEVICEID, NULL);
	if (!pcidev) {
		pr_warn("wixlcm: ICH8 LPC device %04X:%04X not found, buttons disabled\n",
			LPC_VENDORID, LPC_DEVICEID);
		return -ENODEV;
	}

	ret = pci_read_config_dword(pcidev, LPC_OFFSET_GPIOBASE, &gpiobase);
	if (ret || !(gpiobase & 0x01)) {
		pr_warn("wixlcm: PCH GPIO base unavailable, buttons disabled\n");
		pci_dev_put(pcidev);
		return -ENODEV;
	}
	pch_gpio_base = gpiobase & 0x0000FF80u;
	pci_dev_put(pcidev);

	/* GPIO function + inputs; disable input inversion (read raw level). */
	outl(inl(pch_gpio_base + PCH_GPIO_USE_SEL) | pin_mask,
	     pch_gpio_base + PCH_GPIO_USE_SEL);
	outl(inl(pch_gpio_base + PCH_GPIO_IO_SEL) | pin_mask,
	     pch_gpio_base + PCH_GPIO_IO_SEL);
	outl(inl(pch_gpio_base + PCH_GPIO_INV) & ~pin_mask,
	     pch_gpio_base + PCH_GPIO_INV);

	/* seed debounce state from the current (assumed not pressed) level */
	lvl = inl(pch_gpio_base + PCH_GPIO_LVL);
	for (i = 0; i < ARRAY_SIZE(buttons); i++) {
		int l = (lvl >> buttons[i]->gpio_idx) & 0x01;

		buttons[i]->idle_level = l;
		buttons[i]->last_raw = l;
		buttons[i]->stable = l;
		buttons[i]->stable_jiffies = jiffies;
	}

	ret = misc_register(&wixbtndev);
	if (ret) {
		pr_err("wixlcm: misc_register(/dev/wixbtn) failed: %d\n", ret);
		pch_gpio_base = 0;
		return ret;
	}
	wixbtn_registered = true;

	timer_setup(&btn_timer, btn_poll_timer, 0);
	btn_timer_on = true;
	mod_timer(&btn_timer, jiffies + msecs_to_jiffies(BTN_POLL_MS));

	pr_info("wixlcm: LCD buttons ready on /dev/%s (polled PCH GPIO%d/GPIO%d, %dms)\n",
		BTN_DEVICE_NAME, BTN_SEL_GPIO_IDX, BTN_SCR_GPIO_IDX,
		BTN_POLL_MS);
	return 0;
}

static void wix_buttons_exit(void)
{
	if (btn_timer_on) {
		btn_timer_on = false;
		wix_timer_delete_sync(&btn_timer);
	}

	if (wixbtn_registered) {
		misc_deregister(&wixbtndev);
		wixbtn_registered = false;
	}

	wake_up_interruptible(&btn_wait);
	wixbtn_cleanup_events();
	pch_gpio_base = 0;
}

#else  /* !CONFIG_PCI */

static int wix_buttons_init(void)
{
	pr_warn("wixlcm: kernel built without PCI, buttons disabled\n");
	return -ENODEV;
}

static void wix_buttons_exit(void)
{
}

#endif /* CONFIG_PCI */

/* ======================================================================= */
/* module init/exit                                                         */
/* ======================================================================= */

static int __init wixlcm_init(void)
{
	int ret;

	if (initial_backlight < WIX_LCM_BL_OFF ||
	    initial_backlight > WIX_LCM_BL_BLINK)
		initial_backlight = WIX_LCM_BL_ON;

	if (!request_region(sioaddr, 2, "wixlcm")) {
		pr_warn("wixlcm: SIO ports 0x%X-0x%X already reserved, continuing anyway\n",
			sioaddr, sioaddr + 1);
	} else {
		region_held = true;
	}

	ret = lcm_hw_detect_and_config();
	if (ret != 0)
		goto err_release_region;

	if (initialize) {
		mutex_lock(&lcm_lock);
		initialize_lcm();
		mutex_unlock(&lcm_lock);
	}

	ret = misc_register(&lcmdev);
	if (ret != 0) {
		pr_err("wixlcm: misc_register(/dev/lcm) failed: %d\n", ret);
		goto err_release_region;
	}

	device_create_file(lcmdev.this_device, &dev_attr_backlight);

	/* backlight hardware + initial state (failure is non-fatal) */
	if (backlight_hw_probe() == 0) {
		backlight_set_state(initial_backlight);

#if IS_ENABLED(CONFIG_BACKLIGHT_CLASS_DEVICE)
		{
			struct backlight_properties props;

			memset(&props, 0, sizeof(props));
			props.type = BACKLIGHT_RAW;
			props.max_brightness = WIX_LCM_BL_BLINK;
			props.brightness = backlight_state;
			props.power = FB_BLANK_UNBLANK;
			wix_bl_dev = backlight_device_register("wixlcm",
				lcmdev.this_device, NULL,
				&wix_backlight_ops, &props);
			if (IS_ERR(wix_bl_dev)) {
				pr_warn("wixlcm: backlight class register failed: %ld\n",
					PTR_ERR(wix_bl_dev));
				wix_bl_dev = NULL;
			}
		}
#endif
	}

	/* LCD buttons (failure is non-fatal; the LCM still works) */
	if (enable_buttons)
		wix_buttons_init();

	pr_info("wixlcm: px2-300d front panel driver v%s loaded (sio=0x%X, /dev/%s)\n",
		WIXLCM_VERSION, sioaddr, LCM_DEVICE_NAME);
	return 0;

err_release_region:
	if (region_held) {
		release_region(sioaddr, 2);
		region_held = false;
	}
	return ret;
}

static void __exit wixlcm_exit(void)
{
	wix_buttons_exit();

#if IS_ENABLED(CONFIG_BACKLIGHT_CLASS_DEVICE)
	if (wix_bl_dev) {
		backlight_device_unregister(wix_bl_dev);
		wix_bl_dev = NULL;
	}
#endif

	device_remove_file(lcmdev.this_device, &dev_attr_backlight);
	misc_deregister(&lcmdev);

	/* best effort: turn backlight off and blank the panel */
	if (hwm_idx_port)
		backlight_hw_set(WIX_LCM_BL_OFF);
	mutex_lock(&lcm_lock);
	write_lcm(A0_CMD, LCM_OFF_CMD);
	mutex_unlock(&lcm_lock);

	if (region_held)
		release_region(sioaddr, 2);

	pr_info("wixlcm: driver unloaded\n");
}

module_init(wixlcm_init);
module_exit(wixlcm_exit);

MODULE_AUTHOR("Wistron Corp. (original b2a drivers), standalone adaptation");
MODULE_DESCRIPTION("LenovoEMC px2-300d front panel LCM/backlight/buttons driver");
MODULE_VERSION(WIXLCM_VERSION);
MODULE_LICENSE("GPL");
