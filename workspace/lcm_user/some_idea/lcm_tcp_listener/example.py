#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
example.py - lcm_tcp_listener 发送端示例

把指定字符串（默认 "helloworld"）渲染成 128x64 点阵帧，
base64 编码后封装成 JSON，通过 TCP 发送给 lcm_tcp_listener。

协议详见 lcm_tcp_listener.md：
  每条消息一行一个完整 JSON 对象，以 \\n 结尾；
  frame 字段是 base64 编码的 1024 字节显存（128x64，8页x128列 GRAM 格式）。

用法：
  python3 example.py -h                     # 显示帮助（默认无参数也显示帮助）
  python3 example.py -s                     # 发 "helloworld" 到 127.0.0.1:6776
  python3 example.py -s --text "Hello LCM!" # 换一段文字
  python3 example.py -s -H 192.168.1.10 -p 6776
  python3 example.py -s -c                  # 清屏
  python3 example.py -s -b 1                # 只开背光
"""

import argparse
import base64
import json
import socket
import sys

# =======================================================================
# 屏幕几何常量（与 C 参考实现、lcm_tcp_listener 完全一致）
# =======================================================================
SCREEN_W = 128
SCREEN_H = 64
PAGES = 8
FRAME_SIZE = PAGES * SCREEN_W   # 1024

# 5x7 字体常量
FONT_W = 5
FONT_H = 7
CHAR_W = 6   # 字符步进 = 5 字模 + 1 间距

# =======================================================================
# 内置 5x7 ASCII 点阵字库（列主序，每字节 1 列，bit0=最上一行）
# 覆盖：0-9 A-Z a-z 及常用标点
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
# 帧缓存绘制函数
# =======================================================================
def fb_new():
    """新建一帧全黑显存（1024 字节）。"""
    return bytearray(FRAME_SIZE)


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


def render_text_frame(text):
    """把 text 渲染到 128x64 帧缓存中（居中），返回 1024 字节。"""
    fb = fb_new()
    # 计算文字宽度，居中显示
    text_w = len(text) * CHAR_W
    x = max(0, (SCREEN_W - text_w) // 2)
    y = max(0, (SCREEN_H - FONT_H) // 2)
    fb_draw_text(fb, x, y, text)
    return bytes(fb)


# =======================================================================
# TCP 发送
# =======================================================================
def send_message(host, port, msg):
    """通过 TCP 发送一条 JSON 消息（已含 \\n 结尾）。"""
    with socket.create_connection((host, port), timeout=5) as sock:
        sock.sendall(msg.encode("utf-8"))


# =======================================================================
# 命令行
# =======================================================================
def parse_args(argv=None):
    p = argparse.ArgumentParser(
        description="向 lcm_tcp_listener 发送 128x64 点阵图")
    p.add_argument("-s", "--send", action="store_true",
                   help="执行发送（必须显式指定，否则默认显示帮助）")
    p.add_argument("-H", "--host", default="127.0.0.1",
                   help="监听地址 (默认 127.0.0.1)")
    p.add_argument("-p", "--port", type=int, default=6776,
                   help="监听端口 (默认 6776)")
    p.add_argument("-t", "--text", default="helloworld",
                   help="要显示的字符串 (默认 helloworld)")
    p.add_argument("-c", "--clear", action="store_true", help="清屏")
    p.add_argument("-b", "--backlight", type=int, choices=[0, 1, 2], default=None,
                   help="背光：0 灭 / 1 亮 / 2 闪烁")
    return p.parse_args(argv)


def main():
    args = parse_args()
    if not args.send:
        # 无 -s 时显示帮助并退出
        parse_args(["-h"])
        return

    # 构造 JSON 消息
    obj = {}
    if args.clear:
        obj["clear"] = True
    else:
        frame = render_text_frame(args.text)
        obj["frame"] = base64.b64encode(frame).decode("ascii")
        print("渲染 %d 字节帧，base64 长度 %d 字符"
              % (len(frame), len(obj["frame"])), file=sys.stderr)

    if args.backlight is not None:
        obj["backlight"] = args.backlight

    msg = json.dumps(obj, separators=(",", ":")) + "\n"
    print("发送到 %s:%d -> %s" % (args.host, args.port, msg[:80] + ("..." if len(msg) > 80 else "")),
          file=sys.stderr)

    try:
        send_message(args.host, args.port, msg)
    except ConnectionRefusedError:
        print("连接被拒绝：lcm_tcp_listener 可能未启动或地址/端口不对",
              file=sys.stderr)
        sys.exit(1)
    except OSError as e:
        print("发送失败：%s" % e, file=sys.stderr)
        sys.exit(1)

    print("OK")


if __name__ == "__main__":
    main()
