// SPDX-License-Identifier: GPL-2.0
/*
 * screen_ip_storage.c - px2-300d 前面板 128x64 OLED 状态屏（C 版）
 *
 * 与 ../screen_ip_storage.py 功能完全一致：
 *
 *   E1  192.168.31.200           [#]   E1/E2 标签常显示，IP 查物理网卡，
 *   E2  192.168.124.200         [ ]       [#] 表示接口 up
 *   storage:   [zfspool]               storage 字段常显示，[] 内为 ShowName
 *   [########################          ] ZFS 池 ALLOC/SIZE 的实心像素进度条
 *
 * 节能：
 *   - 缓存上一帧，相同帧不下发；
 *   - 连续 IDLE_OFF_SECONDS（默认 10 秒）无变化后关闭背光；
 *   - 背光关闭期间显存照常更新；按 Select/Scroll 任意一键点亮并维持 10 秒。
 *
 * 系统：Debian 13。直接通过 ioctl 操作 /dev/lcm 与 /dev/wixbtn，
 * 不依赖任何第三方库。
 *
 * 编译：make            运行：sudo ./screen_ip_storage
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "lcm_uapi.h"
#include "glyph.h"

/* =======================================================================
 * 用户可配置常量（按需修改）
 * ======================================================================= */
#define IP1DEV          "enp1s0"
#define IP2DEV          "enp2s0"

#define STORAGE_FS      "ZFS"
#define STORAGE_POOL    "zfspool"
#define STORAGE_SHOWNAME "zfspool"

#define ZPOOL_CMD       "/sbin/zpool"
#define REFRESH_INTERVAL  5      /* 画面刷新间隔（秒） */
#define IDLE_OFF_SECONDS 10     /* 连续多少秒无变化关背光（按任意键恢复） */

#define LCM_DEVICE      "/dev/lcm"
#define BTN_DEVICE      "/dev/wixbtn"

/* ---- 屏幕几何 ---- */
#define SCREEN_W 128
#define SCREEN_H 64
#define NPAGES   8
#define FRAME_SIZE (NPAGES * SCREEN_W)

/* ---- 5x7 字体版面坐标（像素），字模数据见 glyph.h ---- */
#define FONT_W GLYPH_WIDTH
#define FONT_H GLYPH_HEIGHT
#define CHAR_W 6
#define LINE_IP1_Y     1
#define LINE_IP2_Y     18
#define LINE_STORAGE_Y 36
#define BAR_Y          54
#define LABEL_X        0
#define ADDR_X         18
#define STATUS_X       (SCREEN_W - 3 * CHAR_W)   /* 110 */
#define STORAGE_NAME_X 54
#define BAR_FILL_X     7
#define BAR_FILL_Y     55
#define BAR_FILL_H     5
#define BAR_FILL_W     (SCREEN_W - 7 - 7)       /* 114 */

static volatile sig_atomic_t g_running = 1;

static void on_sigint(int sig)
{
	(void)sig;
	g_running = 0;
}

/* =======================================================================
 * 系统信息采集
 * ======================================================================= */
struct iface_info {
	int up;
	char ip[INET_ADDRSTRLEN];
};

struct panel_status {
	struct iface_info ip1;
	struct iface_info ip2;
	const char *storage_name;
	double usage;	/* 0.0 ~ 1.0 */
};

static int get_interface_ip(const char *ifname, char *out, size_t outlen)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct ifreq ifr;
	struct sockaddr_in *sin;

	if (s < 0)
		return -1;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

	if (ioctl(s, SIOCGIFADDR, &ifr) < 0) {
		close(s);
		out[0] = '\0';
		return -1;
	}
	sin = (struct sockaddr_in *)&ifr.ifr_addr;
	if (!inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)outlen))
		out[0] = '\0';
	close(s);
	return 0;
}

static int is_interface_up(const char *ifname)
{
	char path[128];
	char state[16];
	FILE *f;
	int up = 0;
	int s;
	struct ifreq ifr;

	/* 优先读 /sys/class/net/<if>/operstate */
	snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", ifname);
	f = fopen(path, "r");
	if (f) {
		if (fgets(state, sizeof(state), f))
			up = (strncmp(state, "up", 2) == 0);
		fclose(f);
		return up;
	}

	/* 退回 SIOCGIFFLAGS 检查 IFF_UP */
	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return 0;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0)
		up = (ifr.ifr_flags & IFF_UP) ? 1 : 0;
	close(s);
	return up;
}

static void get_interface_info(const char *ifname, struct iface_info *info)
{
	info->up = is_interface_up(ifname);
	if (get_interface_ip(ifname, info->ip, sizeof(info->ip)) != 0)
		info->ip[0] = '\0';
}

static double get_zfs_usage(const char *pool)
{
	char cmd[256];
	FILE *p;
	unsigned long long alloc = 0, size = 0;

	snprintf(cmd, sizeof(cmd), "%s list -H -p -o alloc,size %s 2>/dev/null",
		 ZPOOL_CMD, pool);
	p = popen(cmd, "r");
	if (!p)
		return 0.0;
	if (fscanf(p, "%llu %llu", &alloc, &size) == 2 && size > 0) {
		double r = (double)alloc / (double)size;
		if (r < 0.0)
			r = 0.0;
		if (r > 1.0)
			r = 1.0;
		pclose(p);
		return r;
	}
	pclose(p);
	return 0.0;
}

static void collect_status(struct panel_status *st)
{
	get_interface_info(IP1DEV, &st->ip1);
	get_interface_info(IP2DEV, &st->ip2);
	st->storage_name = STORAGE_SHOWNAME;
	if (strcmp(STORAGE_FS, "ZFS") == 0)
		st->usage = get_zfs_usage(STORAGE_POOL);
	else
		st->usage = 0.0;
}

/* =======================================================================
 * 128x64 显存绘制（8 页 x 128 列，bit=纵向像素）
 * ======================================================================= */
static void fb_new(uint8_t *fb)
{
	memset(fb, 0, FRAME_SIZE);
}

static void fb_set_pixel(uint8_t *fb, int x, int y, int on)
{
	if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H)
		return;
	if (on)
		fb[(y >> 3) * SCREEN_W + x] |= (uint8_t)(1u << (y & 7));
	else
		fb[(y >> 3) * SCREEN_W + x] &= (uint8_t)~(1u << (y & 7));
}

static int fb_draw_char(uint8_t *fb, int x, int y, char ch)
{
	const uint8_t *g = find_glyph(ch);
	int col, row;

	for (col = 0; col < FONT_W; col++) {
		uint8_t bits = g[col];

		for (row = 0; row < FONT_H; row++) {
			if (bits & (1u << row))
				fb_set_pixel(fb, x + col, y + row, 1);
		}
	}
	return x + CHAR_W;
}

static void fb_draw_text(uint8_t *fb, int x, int y, const char *text)
{
	while (*text)
		x = fb_draw_char(fb, x, y, *text++);
}

static void fb_fill_rect(uint8_t *fb, int x0, int y0, int w, int h, int on)
{
	int x, y;

	for (x = x0; x < x0 + w; x++)
		for (y = y0; y < y0 + h; y++)
			fb_set_pixel(fb, x, y, on);
}

static void draw_ip_line(uint8_t *fb, int y, const char *label,
			 const struct iface_info *info)
{
	fb_draw_text(fb, LABEL_X, y, label);
	fb_draw_text(fb, ADDR_X, y, info->ip);
	fb_draw_text(fb, STATUS_X, y, info->up ? "[#]" : "[ ]");
}

static void draw_storage_line(uint8_t *fb, const char *name)
{
	char buf[32];
	int max_chars = (SCREEN_W - STORAGE_NAME_X) / CHAR_W - 2;
	int len = (int)strlen(name);

	fb_draw_text(fb, 0, LINE_STORAGE_Y, "storage:");
	if (len > max_chars)
		len = max_chars;
	snprintf(buf, sizeof(buf), "[%.*s]", len, name);
	fb_draw_text(fb, STORAGE_NAME_X, LINE_STORAGE_Y, buf);
}

static void draw_usage_bar(uint8_t *fb, double ratio)
{
	int fill_w;

	fb_draw_text(fb, 0, BAR_Y, "[");
	fb_draw_text(fb, SCREEN_W - CHAR_W, BAR_Y, "]");

	fill_w = (int)(ratio * BAR_FILL_W + 0.5);
	if (fill_w < 0)
		fill_w = 0;
	if (fill_w > BAR_FILL_W)
		fill_w = BAR_FILL_W;
	if (fill_w > 0)
		fb_fill_rect(fb, BAR_FILL_X, BAR_FILL_Y, fill_w, BAR_FILL_H, 1);
}

static void render_frame(const struct panel_status *st, uint8_t *fb)
{
	fb_new(fb);
	draw_ip_line(fb, LINE_IP1_Y, "E1", &st->ip1);
	draw_ip_line(fb, LINE_IP2_Y, "E2", &st->ip2);
	draw_storage_line(fb, st->storage_name);
	draw_usage_bar(fb, st->usage);
}

/* =======================================================================
 * /dev/lcm、/dev/wixbtn 操作
 * ======================================================================= */
static int lcm_display_cmd(int fd, int cmd)
{
	return ioctl(fd, cmd, 0);
}

static int lcm_set_backlight(int fd, int state)
{
	return ioctl(fd, WIX_LCM_IOC_BL_SET, &state);
}

static int lcm_write_frame(int fd, const uint8_t *fb)
{
	int page;

	for (page = 0; page < NPAGES; page++) {
		struct lcm_info info;

		memset(&info, 0, sizeof(info));
		info.ctrl = WIX_LCM_CMD_RESET;
		info.page = page;
		info.column = 0;
		info.size = SCREEN_W;
		memcpy(info.data, fb + page * SCREEN_W, SCREEN_W);
		if (ioctl(fd, IOCTL_DISPLAY_COMMAND, &info) < 0)
			return -1;
	}
	return 0;
}

/* 等待一个刷新周期；期间按键可读则返回按键码，超时返回 0 */
static int wait_for_key_or_timeout(int btn_fd, int seconds)
{
	fd_set rfds;
	struct timeval tv;
	int key = 0;

	if (btn_fd < 0) {
		sleep(seconds);
		return 0;
	}

	FD_ZERO(&rfds);
	FD_SET(btn_fd, &rfds);
	tv.tv_sec = seconds;
	tv.tv_usec = 0;

	if (select(btn_fd + 1, &rfds, NULL, NULL, &tv) > 0) {
		int code;

		if (read(btn_fd, &code, sizeof(code)) == (ssize_t)sizeof(code))
			key = code;
	}
	return key;
}

/* =======================================================================
 * main
 * ======================================================================= */
int main(void)
{
	int lcd_fd, btn_fd = -1;
	uint8_t frame[FRAME_SIZE];
	uint8_t last_frame[FRAME_SIZE];
	int have_last = 0;
	int unchanged = 0;
	int backlight_on = 1;
	const int idle_cycles = IDLE_OFF_SECONDS / REFRESH_INTERVAL > 0 ?
		IDLE_OFF_SECONDS / REFRESH_INTERVAL : 1;

	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);

	lcd_fd = open(LCM_DEVICE, O_RDWR);
	if (lcd_fd < 0) {
		fprintf(stderr, "无法打开 %s: %s。请确认已 insmod wixlcm.ko 并以 root 运行。\n",
			LCM_DEVICE, strerror(errno));
		return 1;
	}

	btn_fd = open(BTN_DEVICE, O_RDWR);
	if (btn_fd < 0)
		fprintf(stderr, "提示：未打开 %s，按键唤醒功能不可用\n", BTN_DEVICE);

	lcm_set_backlight(lcd_fd, WIX_LCM_BL_ON);
	lcm_display_cmd(lcd_fd, WIX_LCM_IOC_PON);

	printf("状态屏启动(C)：%s/%s，存储 %s；每 %d 秒刷新，画面 %d 秒无变化关背光，"
	       "按任意键点亮维持 %d 秒，Ctrl-C 退出\n",
	       IP1DEV, IP2DEV, STORAGE_SHOWNAME,
	       REFRESH_INTERVAL, IDLE_OFF_SECONDS, IDLE_OFF_SECONDS);

	while (g_running) {
		struct panel_status st;
		int changed, key;

		collect_status(&st);
		render_frame(&st, frame);

		changed = (!have_last || memcmp(frame, last_frame, FRAME_SIZE) != 0);
		if (changed) {
			if (lcm_write_frame(lcd_fd, frame) < 0)
				perror("write_frame");
			memcpy(last_frame, frame, FRAME_SIZE);
			have_last = 1;
			unchanged = 0;
		} else {
			unchanged++;
		}

		printf("E1 %-15s [%s]   E2 %-15s [%s]   %s ALLOC/SIZE=%.1f%%\n",
		       st.ip1.ip[0] ? st.ip1.ip : "-",
		       st.ip1.up ? "#" : " ",
		       st.ip2.ip[0] ? st.ip2.ip : "-",
		       st.ip2.up ? "#" : " ",
		       st.storage_name, st.usage * 100.0);
		fflush(stdout);

		if (!changed && backlight_on && unchanged >= idle_cycles) {
			lcm_set_backlight(lcd_fd, WIX_LCM_BL_OFF);
			backlight_on = 0;
			printf("画面连续 %d 秒无变化，背光已关闭（按任意键点亮）\n",
			       IDLE_OFF_SECONDS);
		}

		key = wait_for_key_or_timeout(btn_fd, REFRESH_INTERVAL);
		if (key != 0) {
			if (!backlight_on) {
				lcm_set_backlight(lcd_fd, WIX_LCM_BL_ON);
				backlight_on = 1;
				printf("按键(%d)唤醒，背光已点亮\n", key);
			}
			unchanged = 0;	/* 重新计时，再维持 IDLE_OFF_SECONDS 秒 */
		}
	}

	printf("\n退出，关闭背光与屏幕 ...\n");
	lcm_set_backlight(lcd_fd, WIX_LCM_BL_OFF);
	lcm_display_cmd(lcd_fd, WIX_LCM_IOC_POFF);
	if (btn_fd >= 0)
		close(btn_fd);
	close(lcd_fd);
	return 0;
}
