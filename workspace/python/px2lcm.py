#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
px2lcm.py - LenovoEMC px2-300d 前面板【通用接口层】

本文件是整个工程中唯一直接操作硬件（/dev/lcm、/dev/wixbtn 的 ioctl）
的模块，只提供"能力"，不包含任何具体业务动作。业务/操作逻辑一律写在
独立的应用文件里，通过 import 引用本模块并【重写回调函数】实现：

    硬件能力（本文件）              应用操作（examples/*.py）
    ----------------------          ----------------------------
    PX2LCM    屏幕/背光原语          FrontPanel.on_select/on_scroll ...
    WixButtons 按键读取/监听线程     回调里决定屏幕显示什么

三种使用方式由浅入深：

    # 方式 1：直接调用原语
    lcd = PX2LCM()
    lcd.backlight_on(); lcd.all_white()

    # 方式 2：给按键监听器挂一个 callback 函数
    btn = WixButtons()
    btn.callback = lambda code: print("按键:", code)
    btn.start()          # 后台守护线程，按下即回调

    # 方式 3（推荐）：继承 FrontPanel，重写回调
    class MyApp(FrontPanel):
        def on_start(self):
            self.lcd.backlight_on()
        def on_select(self):
            self.lcd.all_white()
        def on_scroll(self):
            self.lcd.stripes()
    MyApp().run()        # Ctrl-C 退出，自动关闭设备

Python 2.7 / Python 3 通用，仅依赖标准库（fcntl/struct/threading）。
"""

from __future__ import print_function

import os
import time
import fcntl
import struct
import array
import select
import threading

# =======================================================================
# ioctl 编码（与内核 lcm_uapi.h 保持一致）
# Linux: _IOC(dir, type, nr, size) = (dir<<30)|(size<<16)|(type<<8)|nr
# =======================================================================
_IOC_NONE = 0
_IOC_WRITE = 1
_IOC_READ = 2


def _IOC(direction, ioctype, nr, size):
    return (direction << 30) | (size << 16) | (ioctype << 8) | nr


def _IO(ioctype, nr):
    return _IOC(_IOC_NONE, ioctype, nr, 0)


def _IOR(ioctype, nr, size):
    return _IOC(_IOC_READ, ioctype, nr, size)


def _IOW(ioctype, nr, size):
    return _IOC(_IOC_WRITE, ioctype, nr, size)


def _IOWR(ioctype, nr, size):
    return _IOC(_IOC_READ | _IOC_WRITE, ioctype, nr, size)


# ---- /dev/lcm：屏幕 + 背光 ----
LCM_IOC_MAGIC = 0xDD
IOCTL_DISPLAY_COMMAND = _IO(LCM_IOC_MAGIC, 1)
WIX_LCM_IOC_PON = _IO(LCM_IOC_MAGIC, 7)
WIX_LCM_IOC_POFF = _IO(LCM_IOC_MAGIC, 8)
WIX_LCM_IOC_BL_SET = _IOW(LCM_IOC_MAGIC, 10, 4)
WIX_LCM_IOC_BL_GET = _IOR(LCM_IOC_MAGIC, 11, 4)

WIX_LCM_CMD_PON = 7
WIX_LCM_CMD_POFF = 8
WIX_LCM_CMD_RESET = 9

BACKLIGHT_OFF = 0
BACKLIGHT_ON = 1
BACKLIGHT_BLINK = 2

# ---- 屏幕几何 ----
PAGE_COUNT = 8
COLUMN_COUNT = 128
PAGE_BYTES = 128

# struct lcm_info { int ctrl,page,column,size; char data[128]; } = 144 字节
_STRUCT_HEAD = "iiii"
_STRUCT_SIZE = struct.calcsize(_STRUCT_HEAD) + PAGE_BYTES

# ---- /dev/wixbtn：按键（原厂 nas.h ABI）----
BTN_IOC_MAGIC = ord('c')
READ_BUTTON_NONBLOCKING = _IOWR(BTN_IOC_MAGIC, 150, 4)
READ_BUTTON_BLOCKING = _IOWR(BTN_IOC_MAGIC, 151, 4)

WIX_NO_BTN = 0
WIX_BTN_POWER = 1
WIX_BTN_RESET_BRIEFLY = 2
WIX_BTN_RESET_LONG = 3
WIX_BTN_LCM_SELECT = 4
WIX_BTN_LCM_SCROLL = 5

BUTTON_NAMES = {
    WIX_BTN_POWER: "POWER",
    WIX_BTN_RESET_BRIEFLY: "RESET_SHORT",
    WIX_BTN_RESET_LONG: "RESET_LONG",
    WIX_BTN_LCM_SELECT: "SELECT",
    WIX_BTN_LCM_SCROLL: "SCROLL",
}


class LCMError(IOError):
    """前面板设备操作异常。"""
    pass


# =======================================================================
# 第一层：屏幕 + 背光硬件原语
# =======================================================================
class PX2LCM(object):
    """
    /dev/lcm 的薄封装：显示开关、背光三态、按页/整屏写显存。
    只做"把数据送进硬件"，不规定应用该显示什么。
    """

    WIDTH = COLUMN_COUNT
    HEIGHT = 64
    PAGES = PAGE_COUNT

    def __init__(self, device="/dev/lcm"):
        self.device = device
        self.fd = -1
        try:
            self.fd = os.open(device, os.O_RDWR)
        except OSError as exc:
            raise LCMError(
                "无法打开 %s: %s。请确认已 insmod wixlcm.ko 并以 root 运行。"
                % (device, exc))

    # ---- 资源管理 ----
    def close(self):
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False

    # ---- 内部：下发一帧 ioctl ----
    def _command(self, ctrl, page=0, column=0, payload=b""):
        size = len(payload)
        if not 0 <= page < PAGE_COUNT:
            raise ValueError("page 必须在 0~%d 之间" % (PAGE_COUNT - 1))
        if not 0 <= column < COLUMN_COUNT:
            raise ValueError("column 必须在 0~%d 之间" % (COLUMN_COUNT - 1))
        if size > PAGE_BYTES or column + size > COLUMN_COUNT:
            raise ValueError("一次最多写 %d 字节且 column+size<=%d"
                             % (PAGE_BYTES, COLUMN_COUNT))

        buf = bytearray(_STRUCT_SIZE)
        struct.pack_into(_STRUCT_HEAD, buf, 0, ctrl, page, column, size)
        if size:
            buf[16:16 + size] = bytearray(payload)
        try:
            fcntl.ioctl(self.fd, IOCTL_DISPLAY_COMMAND, buf, 1)
        except IOError as exc:
            raise LCMError("ioctl 失败: %s" % exc)

    # ---- 显示开关（0xAF / 0xAE）----
    def display_on(self):
        self._command(WIX_LCM_CMD_PON)

    def display_off(self):
        self._command(WIX_LCM_CMD_POFF)

    # ---- 背光（0xC0 灭 / 0xD5 亮 / 0xFF 闪）----
    def set_backlight(self, state):
        if state not in (BACKLIGHT_OFF, BACKLIGHT_ON, BACKLIGHT_BLINK):
            raise ValueError("背光状态只能是 0(灭)/1(亮)/2(闪烁)")
        arg = array.array('i', [state])
        try:
            fcntl.ioctl(self.fd, WIX_LCM_IOC_BL_SET, arg, 1)
        except IOError as exc:
            raise LCMError("设置背光失败: %s" % exc)

    def get_backlight(self):
        arg = array.array('i', [0])
        try:
            fcntl.ioctl(self.fd, WIX_LCM_IOC_BL_GET, arg, 1)
        except IOError as exc:
            raise LCMError("读取背光失败: %s" % exc)
        return arg[0]

    def backlight_on(self):
        self.set_backlight(BACKLIGHT_ON)

    def backlight_off(self):
        self.set_backlight(BACKLIGHT_OFF)

    def backlight_blink(self):
        self.set_backlight(BACKLIGHT_BLINK)

    # ---- 显存写入 ----
    def write_page(self, page, data, column=0):
        """写一个 page（8 像素高的水平条带），data 最长 128 字节。"""
        self._command(WIX_LCM_CMD_RESET, page, column, data)

    def write_frame(self, frame):
        """写整屏 1024 字节（8 页 x 128 字节）。"""
        if len(frame) != PAGE_COUNT * PAGE_BYTES:
            raise ValueError("整屏数据必须为 %d 字节"
                             % (PAGE_COUNT * PAGE_BYTES))
        raw = bytearray(frame)
        for page in range(PAGE_COUNT):
            start = page * PAGE_BYTES
            self.write_page(page, bytes(raw[start:start + PAGE_BYTES]))

    def fill(self, value=0x00):
        """整屏填充同一字节：0x00 全黑 / 0xFF 全亮 / 0xAA 纵向隔点。"""
        value &= 0xFF
        row = bytes(bytearray([value]) * PAGE_BYTES)
        for page in range(PAGE_COUNT):
            self.write_page(page, row)

    def clear(self):
        """清屏（全黑）。"""
        self.fill(0x00)

    def all_white(self):
        """全屏点亮并打开显示。"""
        self.fill(0xFF)
        self.display_on()

    def all_black(self):
        """全屏熄灭（清显存）并打开显示。"""
        self.fill(0x00)
        self.display_on()

    def stripes(self):
        """128 列间隔 1 亮 1 黑（竖向条纹），写满 8 页并打开显示。"""
        row = bytes(bytearray(
            [0xFF if (col & 1) else 0x00 for col in range(PAGE_BYTES)]))
        for page in range(PAGE_COUNT):
            self.write_page(page, row)
        self.display_on()


# =======================================================================
# 第一层：按键原语（一次性读取 + 回调监听线程）
# =======================================================================
class WixButtons(object):
    """
    /dev/wixbtn 封装。每按一下上报一个码（按下时上报）：
        4 = Select，5 = Scroll

    提供两种用法：
      A. 主动读取：read() / read_name() / wait_event(timeout)
      B. 回调监听：把函数赋给 self.callback（或子类重写 on_button），
         然后 start()，内部守护线程阻塞读键并回调；stop() 结束。
    """

    SELECT = WIX_BTN_LCM_SELECT
    SCROLL = WIX_BTN_LCM_SCROLL

    def __init__(self, device="/dev/wixbtn"):
        self.device = device
        self.fd = -1
        try:
            self.fd = os.open(device, os.O_RDWR)
        except OSError as exc:
            raise LCMError(
                "无法打开 %s: %s。请确认模块以 enable_buttons=1 加载。"
                % (device, exc))

        # 应用可直接赋值：btn.callback = my_func  # my_func(code)
        self.callback = None
        self._thread = None
        self._running = False
        self._lock = threading.Lock()

    # ---- 资源管理 ----
    def close(self):
        self.stop()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False

    # ---- 方式 A：主动读取 ----
    def read(self, blocking=True):
        """读一个按键码；非阻塞且无键时返回 WIX_NO_BTN(0)。"""
        arg = array.array('i', [WIX_NO_BTN])
        request = READ_BUTTON_BLOCKING if blocking \
            else READ_BUTTON_NONBLOCKING
        try:
            fcntl.ioctl(self.fd, request, arg, 1)
        except IOError as exc:
            raise LCMError("读取按键失败: %s" % exc)
        return arg[0]

    def read_name(self, blocking=True):
        """同 read()，返回名称字符串（无键为 'NONE'）。"""
        return BUTTON_NAMES.get(self.read(blocking), "NONE")

    def wait_event(self, timeout=None):
        """select/poll 等待按键；返回按键码，超时返回 None。"""
        readable, _, _ = select.select([self.fd], [], [], timeout)
        if not readable:
            return None
        data = os.read(self.fd, 4)
        if len(data) == 4:
            return struct.unpack('i', data)[0]
        return None

    # ---- 方式 B：回调监听 ----
    def on_button(self, code):
        """
        按键回调：子类重写本方法，或直接给 self.callback 赋一个
        fn(code) 函数。默认实现转调 self.callback。
        注意：本方法运行在监听线程中，不宜做长时间阻塞操作。
        """
        if self.callback is not None:
            self.callback(code)

    def start(self):
        """启动后台守护线程监听按键（重复调用安全）。"""
        with self._lock:
            if self._running:
                return
            self._running = True
            self._thread = threading.Thread(
                target=self._listen_loop, name="wixbtn-listener")
            self._thread.daemon = True
            self._thread.start()

    def stop(self):
        """停止监听（关闭 fd 会让阻塞中的读线程立即退出）。"""
        with self._lock:
            if not self._running:
                return
            self._running = False
            fd, self.fd = self.fd, -1
        if fd >= 0:
            try:
                os.close(fd)   # 使监听线程的 os.read 抛异常退出
            except OSError:
                pass
        if self._thread is not None:
            self._thread.join(timeout=2)
            self._thread = None

    def is_running(self):
        return self._running

    def _listen_loop(self):
        while self._running and self.fd >= 0:
            try:
                data = os.read(self.fd, 4)
            except OSError:
                break
            if not data or len(data) != 4:
                break
            code = struct.unpack('i', data)[0]
            try:
                self.on_button(code)
            except Exception as exc:  # 回调异常不能杀掉监听线程
                print("px2lcm: 按键回调异常: %s" % exc)


# =======================================================================
# 第二层：应用基类（能力与操作分离的分界线）
# =======================================================================
class FrontPanel(object):
    """
    前面板应用基类：组合 LCD/背光 (self.lcd) 与按键监听 (self.buttons)，
    把"按键 -> 做什么"完全交给子类通过重写回调决定。

    可重写的回调钩子（默认空实现 / 基础分派）：
        on_start()           启动后、进入事件循环前调用一次
        on_button(code)      任意按键，默认分派给下面两个
        on_select()          Select 键（4）
        on_scroll()          Scroll 键（5）
        on_stop()            退出时（设备关闭前）调用一次

    最小例子见 examples/demo_callback.py。
    """

    def __init__(self, lcm_device="/dev/lcm", btn_device="/dev/wixbtn",
                 use_buttons=True):
        self.lcd = PX2LCM(lcm_device)
        self.buttons = WixButtons(btn_device) if use_buttons else None
        self._stopped = False

    # ---- 应用层重写以下回调 ----
    def on_start(self):
        """设备就绪后调用一次：可在这里开背光、画初始画面。"""
        pass

    def on_button(self, code):
        """所有按键的统一入口；默认按键码分派，子类可整体重写。"""
        if code == WIX_BTN_LCM_SELECT:
            self.on_select()
        elif code == WIX_BTN_LCM_SCROLL:
            self.on_scroll()

    def on_select(self):
        """按下 Select(选择) 键时做什么，子类重写。"""
        pass

    def on_scroll(self):
        """按下 Scroll(滚动) 键时做什么，子类重写。"""
        pass

    def on_stop(self):
        """退出清理（此时设备仍可操作），子类重写。"""
        pass

    # ---- 生命周期框架（一般无需重写）----
    def start(self):
        """初始化回调监听并触发 on_start，不阻塞。"""
        self.on_start()
        if self.buttons is not None:
            # 用绑定方法桥接：线程 -> on_button -> on_select/on_scroll
            self.buttons.on_button = self.on_button
            self.buttons.start()

    def run(self, poll_interval=1.0):
        """启动并阻塞在事件循环，Ctrl-C 优雅退出。"""
        self.start()
        try:
            while True:
                time.sleep(poll_interval)
        except KeyboardInterrupt:
            print("\n收到退出信号，正在结束 ...")
        finally:
            self.stop()

    def stop(self):
        """停止监听、回调 on_stop、关闭设备；重复调用安全。"""
        if self._stopped:
            return
        self._stopped = True
        if self.buttons is not None:
            self.buttons.stop()
        try:
            self.on_stop()
        finally:
            self.lcd.close()


# =======================================================================
# 自检：python px2lcm.py
# 仅做开背光/开屏/全亮/全黑/条纹/关屏的原语冒烟测试，不含业务逻辑。
# =======================================================================
if __name__ == "__main__":
    with PX2LCM() as lcd:
        print("背光 + 开屏 ...")
        lcd.backlight_on()
        lcd.display_on()
        time.sleep(1)
        print("全亮 ...")
        lcd.all_white()
        time.sleep(2)
        print("全黑 ...")
        lcd.all_black()
        time.sleep(2)
        print("间隔条纹 ...")
        lcd.stripes()
        time.sleep(2)
        print("关屏、背光灭 ...")
        lcd.backlight_off()
        lcd.display_off()
    print("冒烟测试结束。")
