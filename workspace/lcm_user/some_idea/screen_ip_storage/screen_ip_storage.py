#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
screen_ip_storage.py - px2-300d 前面板 128x64 OLED 状态屏（对应照片布局）

屏幕布局（128 x 64，从上到下四行）：

    E1  192.168.31.200           [#]     <- E1 字段常显示；IP 查物理网卡；
    E2  192.168.124.200          [ ]         [#] 表示接口处于 up 状态
    storage:   [zfspool]                 <- storage 字段常显示；[] 内为
    [########################          ]  <- 选中存储池 ALLOC/SIZE 的实心
                                              像素进度条（非 '#' 字符）

背光节能策略：
  - 自 v1.2 起，**背光自动唤醒 + 10 秒无按键自动熄灭** 完全由内核驱动
    wixlcm.ko 负责，本应用不再自行管理背光开关；
  - 帧缓存比对仍保留（画面完全相同则不下发），减少 I/O；
  - 内核在按键检测到后自动点亮背光并重置 10 秒计时，用户态无需
    打开 /dev/wixbtn 或写任何背光控制代码；
  - 若用户态需要主动控制背光（如展示演示模式），仍可调用
    lcd.backlight_on()/off()——但这会取消内核的自动熄灭计时，
    直到下一次物理按键才重新 arm。

运行：sudo python3 screen_ip_storage.py        （需已 insmod wixlcm.ko v1.2+）
退出：Ctrl-C
"""

import os
import sys
import time
import socket
import struct
import fcntl
import subprocess

# 引用上一级目录中的通用接口 example.py（只它一个文件直接操作 /dev/lcm）
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from example import PX2LCM, LCMError  # noqa: E402

# =======================================================================
# 用户可配置常量（按需修改）
# =======================================================================
IP1DEV = "enp1s0"
IP2DEV = "enp2s0"

# FS: 文件系统类型（目前实现 ZFS）；PoolName: zpool 池名；ShowName: 屏幕显示名
STORAGE_DEV = {"FS": "ZFS", "PoolName": "zfspool", "ShowName": "zfspool"}

ZPOOL_CMD = "/sbin/zpool"   # zpool 可执行文件位置
REFRESH_INTERVAL = 5        # 画面刷新间隔（秒）
# IDLE_OFF_SECONDS —— v1.2 起已由内核驱动 wixlcm.ko 内置自动背光管理，
# 本应用不再自行管理背光。如需改节能超时，insmod 时设 auto_off_ms=<毫秒>。

# ---- 屏幕几何（example 显存：8 页 x 128 列，每字节 8 个纵向像素）----
SCREEN_W = 128
SCREEN_H = 64
PAGES = 8

# ---- 5x7 点阵字体的版面坐标（像素）----
FONT_W = 5          # 字形宽 5 像素
FONT_H = 7          # 字形高 7 像素
CHAR_W = 6          # 含 1 像素字间距的步进
LINE_IP1_Y = 1
LINE_IP2_Y = 18
LINE_STORAGE_Y = 36
BAR_Y = 54          # 进度条括号所在行（54..60）

LABEL_X = 0
ADDR_X = 18                       # "E1 "/"E2 " 后 IP 地址起始列
STATUS_X = SCREEN_W - 3 * CHAR_W  # "[#]"/"[ ]" 右对齐：x=110
STORAGE_LABEL_X = 0
STORAGE_NAME_X = 54               # "[ShowName]" 起始列
BAR_X = 0
BAR_FILL_X = 7                    # 左右括号各占 6 像素后的实心条起点
BAR_FILL_W = SCREEN_W - 7 - 7     # 实心条最大宽度 = 114
BAR_FILL_Y = 55                   # 实心条顶行
BAR_FILL_H = 5                    # 实心条高度（像素）

# =======================================================================
# 内置 5x7 ASCII 点阵字库（列主序，每字节 1 列，bit0=最上一行）
# 覆盖：0-9 A-Z a-z 及本屏用到的标点
# =======================================================================
FONT_5X7 = {
    " ": (0x00, 0x00, 0x00, 0x00, 0x00),
    "!": (0x00, 0x00, 0x7F, 0x00, 0x00),
    "#": (0x08, 0x2A, 0x7F, 0x2A, 0x1C),
    "%": (0x23, 0x13, 0x08, 0x64, 0x62),
    "-": (0x08, 0x08, 0x08, 0x08, 0x08),
    ".": (0x00, 0x60, 0x60, 0x00, 0x00),
    "/": (0x20, 0x10, 0x08, 0x04, 0x02),
    ":": (0x00, 0x36, 0x36, 0x00, 0x00),
    "[": (0x00, 0x7F, 0x41, 0x41, 0x00),
    "]": (0x00, 0x41, 0x41, 0x7F, 0x00),
    "_": (0x40, 0x40, 0x40, 0x40, 0x40),
    "0": (0x3E, 0x51, 0x49, 0x45, 0x3E),
    "1": (0x00, 0x42, 0x7F, 0x40, 0x00),
    "2": (0x42, 0x61, 0x51, 0x49, 0x46),
    "3": (0x21, 0x41, 0x45, 0x4B, 0x31),
    "4": (0x18, 0x14, 0x12, 0x7F, 0x10),
    "5": (0x27, 0x45, 0x45, 0x45, 0x39),
    "6": (0x3C, 0x4A, 0x49, 0x49, 0x30),
    "7": (0x01, 0x71, 0x09, 0x05, 0x03),
    "8": (0x36, 0x49, 0x49, 0x49, 0x36),
    "9": (0x06, 0x49, 0x49, 0x29, 0x1E),
    "A": (0x7E, 0x11, 0x11, 0x11, 0x7E),
    "B": (0x7F, 0x49, 0x49, 0x49, 0x36),
    "C": (0x3E, 0x41, 0x41, 0x41, 0x22),
    "D": (0x7F, 0x41, 0x41, 0x22, 0x1C),
    "E": (0x7F, 0x49, 0x49, 0x49, 0x41),
    "F": (0x7F, 0x09, 0x09, 0x09, 0x01),
    "G": (0x3E, 0x41, 0x49, 0x49, 0x7A),
    "H": (0x7F, 0x08, 0x08, 0x08, 0x7F),
    "I": (0x00, 0x41, 0x7F, 0x41, 0x00),
    "J": (0x20, 0x40, 0x41, 0x3F, 0x01),
    "K": (0x7F, 0x08, 0x14, 0x22, 0x41),
    "L": (0x7F, 0x40, 0x40, 0x40, 0x40),
    "M": (0x7F, 0x02, 0x0C, 0x02, 0x7F),
    "N": (0x7F, 0x04, 0x08, 0x10, 0x7F),
    "O": (0x3E, 0x41, 0x41, 0x41, 0x3E),
    "P": (0x7F, 0x09, 0x09, 0x09, 0x06),
    "Q": (0x3E, 0x41, 0x51, 0x21, 0x5E),
    "R": (0x7F, 0x09, 0x19, 0x29, 0x46),
    "S": (0x46, 0x49, 0x49, 0x49, 0x31),
    "T": (0x01, 0x01, 0x7F, 0x01, 0x01),
    "U": (0x3F, 0x40, 0x40, 0x40, 0x3F),
    "V": (0x1F, 0x20, 0x40, 0x20, 0x1F),
    "W": (0x3F, 0x40, 0x38, 0x40, 0x3F),
    "X": (0x63, 0x14, 0x08, 0x14, 0x63),
    "Y": (0x07, 0x08, 0x70, 0x08, 0x07),
    "Z": (0x61, 0x51, 0x49, 0x45, 0x43),
    "a": (0x20, 0x54, 0x54, 0x54, 0x78),
    "b": (0x7F, 0x48, 0x44, 0x44, 0x38),
    "c": (0x38, 0x44, 0x44, 0x44, 0x20),
    "d": (0x38, 0x44, 0x44, 0x48, 0x7F),
    "e": (0x38, 0x54, 0x54, 0x54, 0x18),
    "f": (0x08, 0x7E, 0x09, 0x01, 0x02),
    "g": (0x18, 0xA4, 0xA4, 0xA4, 0x7C),
    "h": (0x7F, 0x08, 0x04, 0x04, 0x78),
    "i": (0x00, 0x44, 0x7D, 0x40, 0x00),
    "j": (0x40, 0x80, 0x84, 0x7D, 0x00),
    "k": (0x7F, 0x10, 0x28, 0x44, 0x00),
    "l": (0x00, 0x41, 0x7F, 0x40, 0x00),
    "m": (0x7C, 0x04, 0x18, 0x04, 0x78),
    "n": (0x7C, 0x08, 0x04, 0x04, 0x78),
    "o": (0x38, 0x44, 0x44, 0x44, 0x38),
    "p": (0x7C, 0x14, 0x14, 0x14, 0x08),
    "q": (0x08, 0x14, 0x14, 0x18, 0x7C),
    "r": (0x7C, 0x08, 0x04, 0x04, 0x08),
    "s": (0x48, 0x54, 0x54, 0x54, 0x20),
    "t": (0x04, 0x3F, 0x44, 0x40, 0x20),
    "u": (0x3C, 0x40, 0x40, 0x20, 0x7C),
    "v": (0x1C, 0x20, 0x40, 0x20, 0x1C),
    "w": (0x3C, 0x40, 0x20, 0x40, 0x3C),
    "x": (0x44, 0x28, 0x10, 0x28, 0x44),
    "y": (0x1C, 0xA0, 0xA0, 0xA0, 0x7C),
    "z": (0x44, 0x64, 0x54, 0x4C, 0x44),
}


# =======================================================================
# 系统信息采集函数
# =======================================================================
def get_interface_ip(ifname):
    """
    查询物理网卡上配置的 IPv4 地址（SIOCGIFADDR）。
    无地址/接口不存在时返回空字符串。
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        buf = fcntl.ioctl(sock.fileno(), 0x8915,  # SIOCGIFADDR
                          struct.pack("256s", ifname[:15].encode("utf-8")))
        return socket.inet_ntoa(buf[20:24])
    except OSError:
        return ""
    finally:
        sock.close()


def is_interface_up(ifname):
    """
    判断接口是否处于启动状态。优先读 /sys/class/net/<if>/operstate，
    sysfs 不可用时退回 SIOCGIFFLAGS 检查 IFF_UP。
    """
    state_file = "/sys/class/net/%s/operstate" % ifname
    try:
        with open(state_file, "r") as f:
            return f.read().strip() == "up"
    except IOError:
        pass

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        buf = fcntl.ioctl(sock.fileno(), 0x8913,  # SIOCGIFFLAGS
                          struct.pack("256s", ifname[:15].encode("utf-8")))
        flags = struct.unpack("H", buf[16:18])[0]
        return bool(flags & 0x1)   # IFF_UP
    except OSError:
        return False
    finally:
        sock.close()


def get_interface_info(ifname):
    """汇总一块网卡的 (是否up, IPv4字符串)。"""
    return {
        "up": is_interface_up(ifname),
        "ip": get_interface_ip(ifname),
    }


def get_zfs_usage(pool):
    """
    用 /sbin/zpool list 读取存储池 ALLOC/SIZE，返回已用比例 0.0~1.0。
    -H: 去表头脚本模式；-p: 输出精确字节数；-o alloc,size 只要两列。
    任何异常（未装 zfs / 池不存在）都返回 0.0。
    """
    try:
        out = subprocess.check_output(
            [ZPOOL_CMD, "list", "-H", "-p", "-o", "alloc,size", pool],
            stderr=subprocess.STDOUT)
        fields = out.decode("utf-8", "replace").split()
        if len(fields) >= 2:
            alloc = int(fields[0])
            size = int(fields[1])
            if size > 0:
                ratio = float(alloc) / float(size)
                return min(1.0, max(0.0, ratio))
    except (OSError, subprocess.CalledProcessError, ValueError):
        pass
    return 0.0


def get_storage_usage(storage_dev):
    """按 STORAGE_DEV['FS'] 分派取用量；目前实现 ZFS。"""
    fs_type = storage_dev.get("FS", "").upper()
    if fs_type == "ZFS":
        return get_zfs_usage(storage_dev["PoolName"])
    return 0.0


def collect_status():
    """采集整屏需要的全部信息。"""
    return {
        "ip1": get_interface_info(IP1DEV),
        "ip2": get_interface_info(IP2DEV),
        "storage_name": STORAGE_DEV.get("ShowName", ""),
        "usage": get_storage_usage(STORAGE_DEV),
    }


# =======================================================================
# 128x64 显存绘制函数（framebuffer = 8 页 x 128 字节，bit=纵向像素）
# =======================================================================
def fb_new():
    """新建一帧全黑显存（1024 字节）。"""
    return bytearray(PAGES * SCREEN_W)


def fb_set_pixel(fb, x, y, on=True):
    """点亮/熄灭一个像素，自动裁剪到屏幕范围。"""
    if 0 <= x < SCREEN_W and 0 <= y < SCREEN_H:
        page = y >> 3
        bit = y & 0x07
        idx = page * SCREEN_W + x
        if on:
            fb[idx] |= (1 << bit)
        else:
            fb[idx] &= ~(1 << bit) & 0xFF


def fb_draw_char(fb, x, y, ch):
    """在 (x,y) 画一个 5x7 字符，返回字符占用后的下一列。"""
    glyph = FONT_5X7.get(ch)
    if glyph is None:
        glyph = FONT_5X7.get(ch.upper(), FONT_5X7[" "])
    for col in range(FONT_W):
        bits = glyph[col]
        for row in range(FONT_H):
            if bits & (1 << row):
                fb_set_pixel(fb, x + col, y + row, True)
    return x + CHAR_W


def fb_draw_text(fb, x, y, text):
    """从 (x,y) 起画一串字符，返回结束列。"""
    for ch in text:
        x = fb_draw_char(fb, x, y, ch)
    return x


def fb_fill_rect(fb, x0, y0, w, h, on=True):
    """填充/清空一个实心矩形（进度条用）。"""
    for x in range(x0, x0 + w):
        for y in range(y0, y0 + h):
            fb_set_pixel(fb, x, y, on)


def draw_ip_line(fb, y, label, info):
    """画一行 "Ex <地址>          [#]"/"[ ]"（label 为 'E1'/'E2'）。"""
    fb_draw_text(fb, LABEL_X, y, label)
    fb_draw_text(fb, ADDR_X, y, info["ip"])                 # 无地址则空
    fb_draw_text(fb, STATUS_X, y, "[#]" if info["up"] else "[ ]")


def draw_storage_line(fb, name):
    """画 "storage:   [ShowName]"，名称过长时截断到括号内放得下。"""
    fb_draw_text(fb, STORAGE_LABEL_X, LINE_STORAGE_Y, "storage:")
    max_chars = (SCREEN_W - STORAGE_NAME_X) // CHAR_W - 2   # 减去 []
    if len(name) > max_chars:
        name = name[:max_chars]
    fb_draw_text(fb, STORAGE_NAME_X, LINE_STORAGE_Y, "[%s]" % name)


def draw_usage_bar(fb, ratio):
    """画 "[实心像素条          ]"，填充宽度按 ALLOC/SIZE 比例计算。"""
    fb_draw_text(fb, BAR_X, BAR_Y, "[")
    fb_draw_text(fb, SCREEN_W - CHAR_W, BAR_Y, "]")
    fill_w = int(round(BAR_FILL_W * ratio))
    fill_w = min(BAR_FILL_W, max(0, fill_w))
    if fill_w > 0:
        fb_fill_rect(fb, BAR_FILL_X, BAR_FILL_Y, fill_w, BAR_FILL_H, True)


def render_frame(status):
    """把采集到的状态渲染成一帧 1024 字节显存。"""
    fb = fb_new()
    draw_ip_line(fb, LINE_IP1_Y, "E1", status["ip1"])
    draw_ip_line(fb, LINE_IP2_Y, "E2", status["ip2"])
    draw_storage_line(fb, status["storage_name"])
    draw_usage_bar(fb, status["usage"])
    return bytes(fb)


# =======================================================================
# 主循环：定时采集 -> 渲染 -> 帧缓存去重后下发；
#         背光自动管理由内核驱动 wixlcm.ko 负责
# =======================================================================
def log_status(status):
    """在终端打印一行当前状态（与屏幕内容一致）。"""
    print("E1 %-15s [%s]   E2 %-15s [%s]   %s ALLOC/SIZE=%.1f%%"
          % (status["ip1"]["ip"] or "-",
             "#" if status["ip1"]["up"] else " ",
             status["ip2"]["ip"] or "-",
             "#" if status["ip2"]["up"] else " ",
             status["storage_name"],
             status["usage"] * 100.0))
    sys.stdout.flush()


def run_loop(lcd):
    """主事件循环，直到 Ctrl-C。

    每 REFRESH_INTERVAL 秒采集一次系统状态，渲染到显存，
    与上一帧比对——逐字节相同则跳过下发以减少 I/O。
    背光的自动唤醒 / 10 秒后自动熄灭由内核驱动 v1.2 内置管理，
    本函数不再调用任何 backlight_on/off。
    """
    print("状态屏启动：%s/%s，存储 %s；每 %d 秒刷新，"
          "背光节能由内核自动管理（默认按键后 10 秒无后续即熄灭），"
          "Ctrl-C 退出"
          % (IP1DEV, IP2DEV, STORAGE_DEV.get("ShowName", ""),
             REFRESH_INTERVAL))

    last_frame = None
    while True:
        status = collect_status()
        frame = render_frame(status)

        if last_frame is None or frame != last_frame:
            lcd.write_frame(frame)
            last_frame = frame

        log_status(status)
        time.sleep(REFRESH_INTERVAL)


def main():
    try:
        lcd = PX2LCM()
    except LCMError as exc:
        print(exc)
        sys.exit(1)

    try:
        # 启动时亮一次背光，让用户立刻看到初始画面；
        # 之后完全交给内核驱动 v1.2 管理（按键自动亮 / 10s 后自动灭）。
        lcd.backlight_on()
        lcd.display_on()
        run_loop(lcd)
    except KeyboardInterrupt:
        print("\n退出 ...")
    finally:
        # 应用进程退出时把背光熄灭，屏幕控制器也关掉——
        # 这是 cleanup，不是节能循环的一部分。
        lcd.backlight_off()
        lcd.display_off()
        lcd.close()


if __name__ == "__main__":
    main()
