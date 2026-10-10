#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
示例 5：间隔 1 亮间隔 1 黑（128 列交替 0x00/0xFF 竖向条纹）
只引用通用接口 px2lcm.PX2LCM，不做任何硬件级操作。
用法：python demo_stripe.py（需 root，且已加载 wixlcm.ko）
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

    lcd.stripes()
    print("间隔 1 亮间隔 1 黑完成（128 列交替亮灭竖条纹）")
    lcd.close()


if __name__ == "__main__":
    main()
