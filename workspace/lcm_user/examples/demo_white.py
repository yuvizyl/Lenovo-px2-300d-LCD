#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
示例 3：全亮 —— 8 页 x 128 列全部写 0xFF
只引用通用接口 px2lcm.PX2LCM，不做任何硬件级操作。
用法：python demo_white.py（需 root，且已加载 wixlcm.ko）
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

    lcd.all_white()
    print("全亮完成（写 0xFF，显示已打开）")
    lcd.close()


if __name__ == "__main__":
    main()
