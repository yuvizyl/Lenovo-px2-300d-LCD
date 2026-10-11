#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
示例 6：按键回调 —— 实现与操作分离的标准写法

通用接口 px2lcm.py 只提供硬件能力（屏幕/背光/按键监听），
本文件只描述"业务操作"：继承 FrontPanel 并重写回调函数，
由框架在按键发生时回调，应用自身不碰任何 ioctl。

按键行为（在下面的回调里自行决定，可任意修改）：
    Select(选择) : 在 全亮 / 全黑 之间切换，并恢复背光常亮
    Scroll(滚动) : 显示间隔 1 亮 1 黑条纹，背光进入硬件闪烁
注意回调运行在监听线程中，应快速返回，不要在里面 sleep 阻塞。

运行：python demo_callback.py（需 root，且已加载 wixlcm.ko）
退出：Ctrl-C（自动关背光、关屏、关闭设备）
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from px2lcm import FrontPanel, BACKLIGHT_BLINK, BACKLIGHT_ON, BUTTON_NAMES


class PanelApp(FrontPanel):
    """业务层：只重写回调，具体动作全部调用 self.lcd 的通用能力。"""

    def on_start(self):
        # 设备就绪后的初始画面
        self.white = False
        self.lcd.backlight_on()
        self.lcd.display_on()
        self.lcd.clear()
        print("就绪：按 Select 切换 全亮/全黑，按 Scroll 显示条纹；Ctrl-C 退出")

    def on_button(self, code):
        # 统一入口：先打印日志，再交回默认分派（-> on_select/on_scroll）
        print("[回调] 收到按键: %s (%d)" % (BUTTON_NAMES.get(code, code), code))
        super(PanelApp, self).on_button(code)

    def on_select(self):
        # 决定 Select 按下后的操作：全亮/全黑翻转，背光恢复常亮
        self.lcd.set_backlight(BACKLIGHT_ON)
        self.white = not self.white
        if self.white:
            self.lcd.all_white()
            print("  -> 全亮，背光常亮")
        else:
            self.lcd.all_black()
            print("  -> 全黑，背光常亮")

    def on_scroll(self):
        # 决定 Scroll 按下后的操作：条纹 + 背光硬件闪烁（立即返回）
        self.lcd.stripes()
        self.lcd.set_backlight(BACKLIGHT_BLINK)
        print("  -> 间隔条纹，背光闪烁")

    def on_stop(self):
        # 退出前的清理动作（此时设备仍可操作）
        self.lcd.backlight_off()
        self.lcd.display_off()
        print("已关背光与屏幕")


def main():
    # ----------------------------------------------------------
    # 写法 A（推荐）：继承重写回调
    # ----------------------------------------------------------
    app = PanelApp()
    app.run()

    # ----------------------------------------------------------
    # 写法 B（不建子类）：直接给 callback 赋函数，等价示意：
    #
    #   from px2lcm import WixButtons, PX2LCM, WIX_BTN_LCM_SELECT
    #   lcd = PX2LCM(); lcd.backlight_on()
    #   btn = WixButtons()
    #   def handle(code):
    #       if code == WIX_BTN_LCM_SELECT:
    #           lcd.all_white()
    #   btn.callback = handle
    #   btn.start()
    #   ...
    #   btn.stop(); lcd.close()
    # ----------------------------------------------------------


if __name__ == "__main__":
    main()
