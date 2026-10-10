#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
示例 1：启动屏幕（0xAF）—— 只引用通用接口 px2lcm.PX2LCM
用法：python demo_on.py（需 root，且已加载 wixlcm.ko）
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from px2lcm import PX2LCM, LCMError


def main():
    try:
        lcd = PX2LCM()
    except LCMError as exc:
        print(exc)
        sys.exit(1)

    lcd.display_on()
    print("屏幕已启动（display ON, 0xAF）")
    lcd.close()


if __name__ == "__main__":
    main()
