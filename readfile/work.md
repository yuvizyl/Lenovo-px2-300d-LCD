# LenovoEMC px2-300d 前 LCD 面板 —— 开源包代码调研记录

> 调研对象：`px2_300d` 文件夹内的 LenovoEMC LifeLine 开源套件（FossKit + Debian7 源码包）
> 调研目标：找出 px2-300d 前面板 LCD 屏幕的操作/驱动代码
> 结论：**找到了完整的内核侧 LCD 驱动、LCD 背光控制和前面板按键驱动**，位于设备平台补丁 `b2a.patch` 中；用户态显示服务为闭源组件，不在开源包内。基于这些源码已在 `workspace/` 二次开发出可在现代内核动态加载的独立驱动与 Python 操作层（见第 11 节）。

---

## 1. 设备与平台补丁的对应关系（为什么 b2a 就是 px2-300d）

开源包 `Lifeline_FossKit/kernel/` 下按设备内部代号存放内核补丁：

| 补丁目录 | 平台 | 对应设备 |
|---|---|---|
| **b2a** | x86，Intel Atom + ICH8/PCH + Nuvoton NCT677xF Super I/O | **px2-300d（2 盘位桌面型）** |
| b4b | x86，Intel Cedarview | px4 桌面型 / 机架式（补丁内有 px4 注释） |
| px4px6 | x86，ICH9 GPIO + Fintek F75393 | px4-300d / px6-300d |
| px4-300r | x86 机架式 | px4-300r |
| r12b | x86 | 其他机架式型号 |
| h4c | ARM，Marvell Armada 370/XP | ARM 架构型号 |

b2a = px2-300d 的证据链：

1. **Makefile 原文注释**：`drivers/b2a/Makefile` 第 2 行写着 `# Kernel configuration file for Px2 based driver`；
2. **硬件吻合**：px2-300d 市售规格为 Intel Atom D525 1.8GHz 双核 + 2GB 内存（x86 平台），b2a 补丁为 x86 架构，含 e1000e（ich8lan）/r8168 网卡驱动、NCT677xF 硬件监控、PCH/SIO GPIO，与该主板完全对应；
3. **ODM 吻合**：驱动版权头为 **Wistron Corp.（纬创，px2-300d 的代工厂）2008–2012**，代码内部代号前缀 `wix`（Wistron 的缩写），LCM 驱动版本字符串 `WIX_LCM_VERSION "0.1.4"`、模块描述 `MODULE_DESCRIPTION("wixlcm")`。

---

## 2. LCD 硬件接口（从驱动代码反推）

### 2.1 屏幕规格

- **128 × 64 单色点阵图形 LCM**（ST7565/UC160x 兼容控制器）；
- 显存分页：**8 个 page（page 0~7），每页 128 列（column 0~127）**，每次最多写 128 字节；
- 控制器命令：开屏 `0xAF`、关屏 `0xAE`；页地址 `0xB0 | page`；列地址高 4 位 `0x10 | (col>>4)`、低 4 位 `0x00 | (col&0x0F)`；
- 上电初始化命令序列（`lcm.c` 中 `initial_lcm_pkg[13]`）：
  `AE A2 A0 C8 A6 40 22 81 3F F8 00 A4 2C`，随后再发 `2E`、`2F`（电源稳压）、`AF`（点亮）。

### 2.2 总线方式：Super I/O GPIO 软件模拟 8080 并行总线

LCD 不走 USB/I2C，而是挂在主板 **Nuvoton NCT6776F 超级 I/O 芯片（配置口地址 `0x4E/0x4F`，芯片 ID 寄存器 0x20 读出应为 `0xC3`）的 GPIO** 上，由内核驱动软件模拟并行时序。

引脚定义（`lcm.c` 开头，`0x0Ggx` 中 g=GPIO 组、x=位）：

| 信号 | GPIO | 宏值 | 说明 |
|---|---|---|---|
| A0 | GP01 | 0x0001 | 0=命令 / 1=数据 |
| CS# | GP02 | 0x0002 | 片选 |
| E | GP03 | 0x0003 | 使能（锁存时钟） |
| RS | GP04 | 0x0004 | （复位/寄存器选择） |
| R/W# | GP05 | 0x0005 | 0=写 / 1=读 |
| D0 | GP06 | 0x0006 | 数据位 0 |
| D1 | GP07 | 0x0007 | 数据位 1 |
| D2 | GP46 | 0x0406 | 数据位 2 |
| D3 | GP87 | 0x0807 | 数据位 3 |
| D4 | GP86 | 0x0806 | 数据位 4 |
| D5 | GP84 | 0x0804 | 数据位 5 |
| D6 | GP81 | 0x0801 | 数据位 6 |
| D7 | GP80 | 0x0800 | 数据位 7 |
| LCM_CTL | GP27 | 0x0207 | LCD 控制 |

GPIO 组到 NCT677xF 逻辑设备/寄存器的映射（`nctGppValueSet/Get`）：

| 组号 | 逻辑设备号(LDN) | 数据寄存器 |
|---|---|---|
| 0x00（GP0x） | 0x08 | 0xE1 |
| 0x02（GP2x） | 0x09 | 0xE1 |
| 0x04（GP4x） | 0x09 | 0xF1 |
| 0x08（GP8x） | 0x07 | 0xE5 |

### 2.3 写时序（`write_lcm()`）

1. 数据总线 GPIO 全部切为输出（`set_databus_mode(OUTPUT)`）；
2. E 拉低 → 逐位输出 8 位数据（D0→D7）→ 按命令/数据设置 A0 → R/W 拉低（写）→ CS 拉低；
3. E 拉高 → `udelay(100)` → E 拉低 → CS 拉高 → `udelay(100)`。
所有 SIO 寄存器访问用全局信号量 `sio_global_lock` 串行化（LCM、LED、风扇等共用 Super I/O）。

---

## 3. 内核驱动结构（补丁新增目录 `drivers/b2a/`）

编译开关 `CONFIG_B2A_API`（Kconfig 中 `default y`），整体编为内核模块/内建 `b2a.o`：

```makefile
obj-$(CONFIG_B2A_API) += b2a.o
b2a-objs += b2am.o lcm.o smbus.o gpiopch.o gpiosio.o hwm.o nct677xf.o led.o wdt.o sci.o
```

补丁还改了内核树自身的两处集成入口（这两个文件不属于 `drivers/b2a/`，
同样在 `b2a.patch` 中，见下方补丁路径；本文不再单独还原任何文件）：

- 补丁中的 `drivers/Kconfig`：相对 linux-3.2 原版（补丁里有完整的
  `drivers/Kconfig.orig` 可对照）只新增一行
  `source "drivers/b2a/Kconfig"`（挂进 Device Drivers 菜单，约第 139 行）；
- 补丁中的 `drivers/Makefile`：相对原版（`drivers/Makefile.orig`）
  只新增一行 `obj-$(CONFIG_B2A_API) += b2a/`（编入内核构建，约第 149 行）。

| 文件 | 与 LCD/面板的关系 | 作用 |
|---|---|---|
| **lcm.c / lcm.h** | ★ LCD 主体 | LCD 初始化、GPIO 模拟总线、`/dev/lcm` 字符设备与 ioctl（571 行） |
| **sci.c / sci.h** | ★ 前面板按键 | Wistron ACPI SCI/GPE 中断处理，注册 `/dev/wixbtn` 上报按键事件 |
| **nas.h** | ★ 接口定义 | 按键码、按键/LCM ioctl 号、`lcm_structure` 显示数据结构、定时开机结构 |
| led.c / led.h | ☆ 面板指示灯 | 电源灯、系统蓝/红灯、硬盘故障红灯（含闪烁定时器） |
| b2am.c / b2am.h | 模块入口 | 初始化顺序：SMBus → PCH GPIO → **LCM** → LED → SCI；注册 sysfs class |
| b2a.h | 公共定义 | `SIO_ADDR 0x4E`、调试打印宏 WIXPRINT |
| nct677xf.c/.h | 底层 | NCT677xF Super I/O 配置（GPIO/风扇/电压/温度） |
| gpiosio.c/.h、gpio.h | 底层 | Super I/O GPIO 读写封装 |
| gpiopch.c/.h | 底层 | Intel PCH（ICH8）GPIO 初始化 |
| smbus.c/.h、hwm.c/.h | 周边 | SMBus 与硬件监控（电压/风扇/温度），LCD 不直接依赖 |
| wdt.c/.h | 周边 | 看门狗 |
| Kconfig / Makefile | 构建 | 菜单 `B2A HW API` 与编译规则 |

初始化入口在 `b2am.c::b2am_init()`：`smbus_init()` → `pchgpio_init()` → **`wixlcm_init()`** → `led_init()` → `wixsci_init()`。

---

## 4. 用户态编程接口：`/dev/lcm`

`lcm.c` 通过 `misc_register()` 注册一个 misc 字符设备：

- 设备节点：**`/dev/lcm`**（minor 动态分配）；
- 打开方式：**独占**，已被打开时再次 open 返回 `-EFAULT`；
- 支持 `unlocked_ioctl`（内核 ≥2.6.36）与旧版 `.ioctl`；
- 操作命令（定义在 `nas.h`，magic = `0xDD`）：

| 宏 | 命令号 | 功能 |
|---|---|---|
| `IOCTL_DISPLAY_COMMAND` | _IO(0xDD, 1) | 写屏 / 开关屏 / 复位（主命令，参数为 `lcm_structure`） |
| `IOCTL_SCHEDULE_POWERON` | _IO(0xDD, 2) | 定时开机（参数 `time_structure`） |
| `WIX_LCM_CMD_PON` | _IO(0xDD, 7) | 开屏（发 0xAF） |
| `WIX_LCM_CMD_POFF` | _IO(0xDD, 8) | 关屏（发 0xAE） |
| `WIX_LCM_CMD_RESET` | _IO(0xDD, 9) | 设置页/列地址并写入一页最多 128 字节点阵数据 |

显示数据结构（`nas.h`）：

```c
typedef struct lcm_info {
    int  ctrl;        // LCM 控制命令（PON/POFF/RESET）
    int  page;        // 页号 0~7（对应 8 个水平像素行，每页 8 像素高）
    int  column;      // 列号 0~127
    int  size;        // 本次数据字节数，最大 128；column+size 不得超过 128
    char data[128];   // 点阵数据，每字节 8 个纵向像素
} lcm_structure;
```

典型写屏流程（用户态伪代码）：

```c
int fd = open("/dev/lcm", O_RDWR);
lcm_structure info = { .ctrl = WIX_LCM_CMD_RESET, .page = 0, .column = 0, .size = 128 };
memcpy(info.data, framebuf_page0, 128);
ioctl(fd, IOCTL_DISPLAY_COMMAND, &info);   // 驱动依次发 0xB0|page、列地址、数据
ioctl(fd, WIX_LCM_CMD_PON);                // 点亮背光/显示
```

驱动对参数有范围校验：page 0~7、column 0~127、size 0~128、`column+size ≤ 128`，非法返回 `-EFAULT`。
字符/汉字的取模渲染由用户态程序完成，内核只负责把字节流送到面板。

---

## 5. 前面板按键接口：`/dev/wixbtn`

`sci.c` 注册字符设备类 `wixbtn`，设备节点 **`/dev/wixbtn`**（Kconfig 说明文字中称 `/dev/buttons`，以代码实际节点名 `/dev/wixbtn` 为准）。

- 中断方式：按键接在 ACPI **SCI（通过 GPE）** 上，驱动注册 `wix_acpi_gpe_handler` 和中断处理 `wixsci_isr`，用工作队列（workqueue/tasklet）去抖并上报事件；
- 事件来源 SCI 索引：`WIXSCI_OFFSET_BTN=0`、`WIXSCI_OFFSET_LCM_SEL=2`、`WIXSCI_OFFSET_LCM_SCR=3`、`WIXSCI_OFFSET_PWR=10`；
- 读取 ioctl（`nas.h`，magic = 'c'）：

| 宏 | 命令号 | 说明 |
|---|---|---|
| `READ_BUTTON_BLOCKING` | _IOWR('c', 151, int) | 阻塞读按键 |
| `READ_BUTTON_NONBLOCKING` | _IOWR('c', 150, int) | 非阻塞读按键 |
| `READ_HWE_BLOCKING` / `READ_HWE_NONBLOCKING` | 153 / 152 | 硬件事件（温度/风扇等） |

- 按键码（`nas.h`）：

| 值 | 宏 | 面板按键 |
|---|---|---|
| 0 | `WIX_NO_BTN` | 无按键 |
| 1 | `WIX_BTN_POWER` | 电源键 |
| 2 | `WIX_BTN_RESET_BRIEFLY` | Reset 短按 |
| 3 | `WIX_BTN_RESET_LONG` | Reset 长按 |
| 4 | `WIX_BTN_LCM_SELECT` | **LCD 旁的"选择/确认"键** |
| 5 | `WIX_BTN_LCM_SCROLL` | **LCD 旁的"滚动/翻页"键** |

即：面板上的两个 LCD 操作键（Select、Scroll）+ 电源键、Reset 长短按均由此驱动上报，用户态守护进程据此切换 LCD 菜单（IP 地址、容量、状态等）。

按键的完整硬件链路（`sci.c::wixsci_init()`，二次开发独立驱动时据此实现）：

1. 按键接在 **Intel ICH8 LPC（PCI 8086:2918）的 PCH GPIO2 / GPIO3** 上，
   PMBASE/GPIOBASE 分别从 PCI 配置偏移 0x40 / 0x48 读出并屏蔽低位；
2. PCI 配置 dword 0xB8 中把这两个 GPIO 的 2 位路由字段都置 2，
   即路由为 ACPI GPI → 对应 **GPE18 / GPE19**；
3. GPIO 设为输入，初始化时置位 INV（输入极性翻转）；按下产生第一次
   GPE 中断，处理程序清 INV 位消除电平触发**并上报按键码**，抬起产生
   第二次中断、重新置位 INV 但不上报——即**每按一下上报一次（按下时）**
   （`gpiopch.h`：USE_SEL 0x00 / IO_SEL 0x04 / LVL 0x0C / INV 0x2C）；
4. 中断注册用 `request_irq(9, ..., IRQF_SHARED)` + `acpi_install_gpe_handler`
   （GPE 号 0x10+GPIO 号，电平触发），事件经 workqueue 去抖后入链表，
   用户态 ioctl 151 阻塞取出。

---

## 6. sysfs 与其他面板部件

`b2am.c` 还注册了两个 sysfs class（也在 `/sys/class/...` 下可读写）：

- **LED class**：`led_brightness`、`pwr_led`（电源灯）、`sys_blue_led`（系统蓝灯）、`sys_red_led`（系统红灯）、`hdd_red_led`（硬盘故障红灯，带 0.25/0.5/1 秒闪烁定时器）；

其中 **`led_brightness`（`sysleddev[0]`，SIO GPIO index 27）就是 LCD 背光**，
不是普通 GPIO，而是走 NCT6776F 逻辑设备 0x0B 的 HWM 间接寄存器
（`gpiosio.c` 中 `siogpdev_read/write`，`gpiosio.h` 定义）：

| 项 | 值 |
|---|---|
| 间接口 | LDN 0x0B 基址 +0x05（index）/ +0x06（data），基址由该 LDN 的 0x60/0x61 读出 |
| BANK 选择 | `BANK_SEL 0x004E` |
| 读寄存器 | `AUXFANOUT_RD 0x0011`（看高 4 位：C=灭/D=亮/F=闪） |
| 写寄存器 | `AUXFANOUT_WR 0x0309` |
| 三态取值 | **0xC0=灭，0xD5=亮，0xFF=闪烁** |

> 前提（实测踩坑）：写占空比寄存器**之前**必须先完成引脚复用初始化，
> 否则引脚仍是普通 GPIO，写 0xC0 只能把背光调暗而无法彻底拉灭：
> ① 全局 CR `0x1B |= 0x40`（GP44/45 多功能）；
> ② LDN 0x0B 的 CR `0xF8 |= 0x07`（AUXFANOUT0/1/2 引脚功能选择，
> 低 3 位置 1 = AUXFAN/PWM 输出）。两步见补丁中
> `drivers/b2a/gpiosio.c::siogphost_probe()`；独立驱动
> `workspace/lcm_driver/wixlcm.c` 已在 `backlight_hw_probe()` 补齐。

- **硬件监控 class**：`sysfanspeed`、`vcore`、`12vin`、`5vin`、`ddr`、`3vin`、`vbat`、`cputemp`、`hddtemp`、`wdt`。

这些与 LCD 共同构成 px2-300d 前面板的完整内核支撑。

---

## 7. 用户态部分的边界（开源包里没有的东西）

- 开源包只包含**内核驱动 + ioctl/字符设备接口**；
- 真正在屏幕上画菜单、渲染字体、响应 Select/Scroll 键切换显示内容的，是 LifeLine 固件里的**专有用户态守护进程**（闭源），不随 FossKit 提供；
- 已扫描套件内 `debiansrc`（Debian 基础根文件系统）的 bin/sbin/usr 全部二进制，**没有任何文件引用 `/dev/lcm` 或 `/dev/wixbtn`**，佐证用户态服务不在该开源包内；
- `lcf`（debiansrc 中的闭源二进制）与 LCD 无关；u-boot、DirectFB、smi-domtool（USB DOM 量产工具）、hdlcdrv/freetype（名称中带 lcd 但为通用软件）均与此前面板无关。

---

## 8. 如何使用这些源码

1. 取内核基线：`Lifeline_FossKit/linux-3.2.40.tar.bz2` 或 `linux-3.8.6.tar.bz2`（补丁中原始文件时间戳为 2013-04-05；另有 Debian7 目录的 linux_3.2.101 源码包）；
2. 解压后二选一集成 b2a 驱动（所有源码只存在于补丁文件 `b2a.patch` 中，
   本目录不再保留单独还原的源文件，需要时从补丁提取）：
   - 整体打补丁：`patch -p1 < b2a.patch`（补丁为 `diff -ur` 格式，共 208 个文件段，
     除 `drivers/b2a/` 外还含 aufs、r8168、e1000e 等整套 px2 平台改动）；
   - **只取 LCD 相关部分（推荐，改动面最小）**：从补丁中提取
     `drivers/b2a/` 的 25 个新增文件放进内核树 `drivers/b2a/`（提取方法见
     第 10 节），再在该内核树的 `drivers/Kconfig` 加一行
     `source "drivers/b2a/Kconfig"`、`drivers/Makefile` 加一行
     `obj-$(CONFIG_B2A_API) += b2a/`（补丁里的 `.orig` 文件可对照）；
3. 内核配置启用 `CONFIG_B2A_API`（Device Drivers → B2A HW API，默认 y，可设 m）；
4. 编译得到 b2a 驱动（内建或 b2a.ko），启动后出现 `/dev/lcm` 与 `/dev/wixbtn`，
   `/sys/class/nasled/led_brightness` 控制 LCD 背光（0/1/2，前提见第 6 节）；
5. 自行编写用户态程序：open `/dev/lcm` → 按 128×64 点阵（8 页 ×128 列）填入字模 → ioctl 写屏；open `/dev/wixbtn` → ioctl 150/151 读按键码。

> 注：若只想在新内核（如 Ubuntu 6.8）上动态加载使用，无需折腾 3.2 内核树，
> 直接用本仓库 `workspace/lcm_driver/` 的独立模块即可，见第 11 节。

---

## 9. 其他机型的同类驱动（同套件内参考，非 px2-300d）

接口设计思路相同（Wistron/EMS 各代平台沿用 `wixlcm` 命名与 ioctl 风格）：

- `Lifeline_FossKit/kernel/b4b/b4b.patch`：`drivers/b4b/lcm.c`、`lcm.h`、`led.c`（Intel Cedarview，px4/机架式，按键 Select/Scroll 定义与 px2 互换）；
- `Lifeline_FossKit/kernel/px4px6/px4px6.patch`：`drivers/gpio/ums8485md.c`（LCM ioctl 处理，命令集更丰富：正常/反显、整屏开关、ADC 选择、输出翻转、写数据等）、`drivers/gpio/pega-btn.c`（按键）、`drivers/gpio/pca9538.c`（I2C 扩展 GPIO）、`drivers/gpio/ich9-gpio.c`；
- `Lifeline_FossKit/kernel/px4-300r/px4-300r.patch`：机架式 px4 的 LCM 处理；
- `Lifeline_FossKit/kernel/h4c/h4c.patch`：ARM Marvell Armada 平台的 `arch/arm/plat-armada/wixlcm.c`。

---

## 10. 本 readfile 目录交付清单

```
readfile/
├── work.md                 本说明文档（全部技术结论都在这里，可脱离源码阅读）
└── b2a.patch               px2-300d 平台完整内核补丁（原始副本，复制自
                            Lifeline_FossKit/kernel/b2a/b2a.patch，共 208 个文件段）
```

> 说明 1：本目录**只保留补丁原件，不另存还原出的源文件**——`drivers/b2a/`
> 下 25 个文件（lcm/sci/nas.h/led/b2am/nct677xf/gpiosio/gpiopch/smbus/hwm/
> wdt 等）全部是补丁中的 `diff --new-file` 段，随时可按下面的命令重新提取，
> 避免同一份代码存两份造成漂移。各文件作用见第 3 节表格。
>
> 说明 2：从补丁提取 `drivers/b2a/` 全部新增文件（在解压好的内核树根目录执行）：
>
> ```sh
> # 方法一：直接打整个补丁（会同时改 aufs/网卡等，共 208 段）
> patch -p1 < /path/to/b2a.patch
>
> # 方法二：只取出 drivers/b2a/ 的新增文件，不动内核其余部分
> awk '/^diff --new-file -ur a\/drivers\/b2a\//{p=1}
>      /^diff --new-file -ur / && $0 !~ /drivers\/b2a\//{p=0}
>      p' /path/to/b2a.patch | filterdiff -i '*/drivers/b2a/*' \
>     | (cd /usr/src/linux-3.2.x && patch -p1)
> # 没有 filterdiff 时（patch 包自带），也可手工：
> grep -n '^diff --new-file -ur a/drivers/b2a/' /path/to/b2a.patch
> # 按列出的行号区间，把各文件 '+' 开头的内容行去掉首字符即为原文件。
> ```
>
> 说明 3：内核树集成只需两行改动：`drivers/Kconfig` 增加
> `source "drivers/b2a/Kconfig"`，`drivers/Makefile` 增加
> `obj-$(CONFIG_B2A_API) += b2a/`；补丁中的 `drivers/Kconfig.orig`、
> `drivers/Makefile.orig` 是 linux-3.2 原始完整文件，可直接对照。
>
> 说明 4：补丁中其余约 180 个文件段（aufs、e1000e、r8168、mm/tty/usb 等
> 平台级改动）与前面板 LCD 无关；内核基线源码包（linux-3.2.40 /
> linux-3.8.6，数百 MB）未复制，需要时从 `Lifeline_FossKit/` 目录取用。
>
> 说明 5：在新内核上使用无需打此补丁，见第 11 节的 `workspace/` 独立驱动。

---

## 11. 二次开发成果：独立可加载驱动 + Python 操作层（`workspace/`）

在上述原厂源码调研基础上，已在仓库 `workspace/` 下产出一套**不依赖
3.2 内核树、可在现代内核（实测 Ubuntu 6.8.0-138-generic 编译通过）上
动态加载**的独立实现，原厂 ABI（ioctl 号、`lcm_structure` 144 字节、
按键码、背光寄存器值）保持二进制兼容。

### 11.1 内核模块 `workspace/lcm_driver/`（单文件模块 wixlcm.ko，v1.1）

| 文件 | 内容 |
|---|---|
| `wixlcm.c` | 独立模块，把原厂十余个 b2a 文件中与面板相关的三条硬件链路收敛到一个文件 |
| `lcm_uapi.h` | 内核/用户态共用 ABI 头（与原厂 `nas.h` 兼容并扩展背光 ioctl） |
| `Makefile` | 树外 Kbuild，支持 `KDIR=/path ARCH=x86 CROSS_COMPILE=...` |
| `load.sh` / `unload.sh` | 编译+insmod/rmmod 脚本，自动建 `/dev/lcm`、`/dev/wixbtn` |

功能与原厂源码的对应关系：

1. **128×64 LCM**（源自 `lcm.c`）：NCT6776F GPIO 软件模拟 8080 总线，
   引脚/逻辑设备/寄存器映射、数据方向掩码、初始化命令序列、100us 锁存
   时序全部逐行沿用；misc 设备 `/dev/lcm`，ioctl `0xDD01` + 扩展
   `0xDD07/0xDD08` 直接开关屏。改进：mutex、规范 errno、`request_region`、
   `compat_ioctl`、芯片 ID 不符时安全拒绝加载。
2. **LCD 背光**（源自 `gpiosio.c` 的 index27/AUXFANOUT）：LDN 0x0B
   间接寄存器 bank 0x4E / reg 0x0309，三态 0xC0/0xD5/0xFF 与原厂一致。
   控制途径：ioctl `0xDD0A/0xDD0B`、`/sys/class/misc/lcm/backlight`、
   标准 `/sys/class/backlight/wixlcm/`（有 CONFIG 时）；模块参数
   `initial_backlight`。
3. **Select/Scroll 两键**（源自 `sci.c`）：PCH GPIO2/3，按下上报 4/5；
   misc 设备 `/dev/wixbtn`，支持原厂 ioctl 150/151、`read()` 取 4 字节
   小端 int、`poll`；模块参数 `enable_buttons=0` 可关。
   **实现方式（实测后改为轮询）**：最初按原厂思路用 PCI 0xB8 路由到
   GPE18/19 + INV 翻转 + `acpi_install_gpe_handler(LEVEL_TRIGGERED)`，
   但电平触发 GPE 在处理期间若未及时撤销会触发 ACPICA 的 GPE-storm
   保护，GPE 被**永久关闭**（现象：第一次按键能唤醒，之后全部失效，
   重启用户程序也无效，只有重载模块恢复）。最终方案：不碰 ACPI/GPE，
   把 GPIO2/3 配成普通输入（清 INV），用内核 `timer_list` 每 20ms
   轮询 PCH GPIO_LVL 寄存器，软件去抖（稳定 60ms 才确认），在"离开
   空闲电平"的按下沿入同一事件队列。无中断风暴、用户程序重启无影响。

### 11.2 Python 层 `workspace/python/`（Python 2.7/3 通用，实现与操作分离）

重构为**单一通用接口文件 + 纯引用它的应用文件**两层结构，只有接口层
直接操作 ioctl：

- `px2lcm.py`（唯一硬件接口层）：
  - `PX2LCM`：屏幕开/关、按页/整屏写显存、填充、背光 on/off/blink/get/set；
  - `WixButtons`：除阻塞/非阻塞读键、select 等待外，新增**回调监听**——
    后台守护线程读 `/dev/wixbtn`，按下即调用 `on_button(code)`
    （子类重写或给 `callback` 赋函数），`start()/stop()` 管理生命周期；
  - `FrontPanel`：应用基类，组合屏幕+背光+按键，提供可重写钩子
    `on_start / on_button / on_select / on_scroll / on_stop` 和
    `run()/stop()` 事件循环（Ctrl-C 优雅退出、重复 stop 安全）。
  - ioctl 编码逐位校验与内核宏一致（`4004DD0A / 8004DD0B / C0046396/7`）。
- `examples/` 六个应用文件，全部只 `import px2lcm`：
  - demo_on/off/white/black/stripe.py：开屏、关屏、全亮、全黑、间隔条纹；
  - demo_callback.py：**回调范式**——继承 `FrontPanel` 重写
    `on_select/on_scroll` 决定屏幕动作（Select 切换全亮/全黑，
    Scroll 条纹+背光闪烁），文件内另附不建子类、直接挂 callback 的写法。
- `screens/` 按"一张屏布局照片对应一个 py 文件"组织的实际界面（Debian 13
  / Python 3）：
  - `screen_ip_storage.py`：仿 LifeLine 状态屏——`E1/E2` 标签常显示
    （无冒号），IP 用 SIOCGIFADDR 查物理网卡（`IP1DEV/IP2DEV` 常量可改），
    右侧 `[#]/[ ]` 由 `/sys/class/net/<if>/operstate` 决定；`storage:`
    常显示，`[]` 内为 `STORAGE_DEV['ShowName']`；最下方为
    `/sbin/zpool list -Hp -o alloc,size` 的 ALLOC/SIZE **实心像素**进度条
    （非 '#' 字符）。文件内置 5x7 ASCII 字库，函数划分"采集/绘制/下发"，
    5 秒刷新；另带节能逻辑：帧缓存去重（相同帧不下发）、画面连续
    `IDLE_OFF_SECONDS`（默认 10 秒）无变化自动关背光，按 Select/Scroll
    任意一键重新点亮并重置 10 秒计时（背光关闭期间显存仍照常更新）。
  - `screens/screen_ip_storage_c/`：上述状态屏的 **C 语言等价实现**
    （`screen_ip_storage.c` + 复制的 `lcm_uapi.h` + Makefile）：直接用
    ioctl 操作 `/dev/lcm`、`/dev/wixbtn`（SIOCGIFADDR / SIOCGIFFLAGS 查网卡、
    `popen("/sbin/zpool list -Hp -o alloc,size")` 查容量、`select` 等键），
    常量同为文件开头的宏，`make && sudo ./screen_ip_storage` 运行。

### 11.3 真机使用

```sh
cd workspace/lcm_driver && sh load.sh     # insmod，出 /dev/lcm /dev/wixbtn
cd ../python
python3 examples/demo_white.py
python3 examples/demo_callback.py         # 常驻：按键回调驱动屏幕，Ctrl-C 退出
insmod wixlcm.ko wixdbg=1                 # 需要时打印每次 GPIO/GPE 操作
```
