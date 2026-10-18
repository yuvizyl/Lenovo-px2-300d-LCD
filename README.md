# px2-300d LCD

LenovoEMC px2-300d 前面板 **128×64 单色 LCD（含背光、Select / Scroll 按键）**
的现代 Linux 驱动与 Python 操作层。

原厂 LifeLine 固件只提供绑定 2013 年内核（Linux 3.2/3.8）的整套平台补丁，
用户态显示程序闭源。本项目把前面板相关逻辑从原厂补丁中剥离、重写为一个
**可在现代内核上动态加载（insmod）的独立内核模块**，并配套零第三方依赖的
Python 库与示例程序，可直接驱动屏幕、背光和两个按键。

> 已在 Debian13 上完成模块编译，硬件寄存器时序与
> 原厂驱动逐行一致。
## 前面板状态：

> sudo python3 workspace/lcm_user/some_idea/screen_ip_storage.py

![screen_ip_storage 实拍前面板效果](readfile/ShowImage/jpeg1.jpg)

## 功能特性

- **128×64 单色 LCM**：整屏 / 按页写入、任意点阵内容（ST7565 兼容控制器）
- **LCD 背光**：亮 / 灭 / 硬件闪烁三态，支持 sysfs、标准 backlight class 与 ioctl；
  **v1.2 内核级自动节能**：加载后背光默认熄灭，按任意物理键自动点亮并在 10 秒无后续按键后自动熄灭（每次按键重置计时）；用户态通过 ioctl / sysfs 主动操作会取消内核自动熄灭计时，避免冲突
- **两个前面板按键**：Select、Scroll，内核 20ms 轮询 PCH GPIO 电平并软件去抖
  （避免 ACPI GPE 电平中断被风暴保护永久关闭），支持阻塞/非阻塞读取、
  `poll` 与 Python 回调
- **动态加载**：单个 `.ko`，无需重编内核；在非 px2-300d 机器上会安全拒绝加载
- **Python 接口层**：硬件能力与业务操作分离，内置 5×7 点阵字库与帧缓存
- **节能状态屏示例**：IP / ZFS 容量显示、相同帧不刷新；背光自动节能（按键唤醒 + 10 秒后自动熄灭）已由内核驱动 v1.2 内置，用户态应用不再自行管理（提供 Python 与 C 两个等价实现）
- 仅依赖 Linux 内核头文件与 Python 标准库（Python 2.7 / 3 通用库接口） 

## 硬件说明

屏幕不是 USB / I2C 设备，而是挂在主板 **Nuvoton NCT6776F 超级 I/O 芯片**上：

- 显示数据通过 Super-I/O GPIO **软件模拟 8080 并行总线**传输；
- 背光走 NCT6776F 的 AUXFANOUT（PWM）引脚；
- Select / Scroll 按键接 Intel ICH8 PCH GPIO2/GPIO3，由驱动轮询电平（不走 ACPI GPE）。

完整的引脚映射、寄存器地址、ioctl ABI、初始化时序、背光复用前提等**技术细节**
请参阅调研文档：[readfile/work.md](readfile/work.md)。

## 目录结构

```
px2_300d_lcd/
├── README.md                     本文件
├── readfile/
│   ├── work.md                   ★ 原厂补丁逆向调研（全部技术细节，建议先读）
|   ├── ShowImage/
│   │   └── jpeg1.jpg             实拍前面板效果
│   ├── b2a.patch                 LenovoEMC 官方开源套件中的原始内核补丁
|   └── display.pptx              前面板显示内容的需求稿（把ppt转成png发给ai让帮忙完成）
│
└── workspace/
    ├── lcm_driver/               独立可加载内核模块
    │   ├── wixlcm.c              驱动主体（LCD + 背光 + 按键，单文件）
    │   ├── lcm_uapi.h            内核/用户态共用接口定义
    │   ├── Makefile              树外 Kbuild
    │   ├── load.sh               编译 + insmod 一键脚本
    │   └── unload.sh             rmmod 卸载脚本
    │
    └── lcm_user/
        ├── example.py                        通用硬件接口层（唯一操作 /dev/lcm 的模块，
        │                                    直接运行可做背光/开屏/全亮/全黑/条纹冒烟自检）
        └── some_idea/
            ├── screen_ip_storage/
            │   └── screen_ip_storage.py         IP / ZFS 状态屏（Python 实际应用）
            ├── screen_ip_storage_c/         同一状态屏的 C 语言实现
            │   ├── screen_ip_storage.c
            │   ├── glyph.h                  5x7 ASCII 点阵字库
            │   ├── lcm_uapi.h               ioctl 接口定义（副本）
            │   └── Makefile
            └── lcm_tcp_listener/            TCP JSON 点阵图监听器（C 语言三线程）
                ├── lcm_tcp_listener.c       主程序（listener / display / watchdog）
                ├── base64.c / base64.h      极简 base64 解码器（无第三方依赖）
                ├── mincjson.c / mincjson.h  极简扁平 JSON 解析器
                ├── lcm_uapi.h               ioctl 接口定义（副本）
                ├── Makefile
                └── lcm_tcp_listener.md      协议说明与使用示例
```

## 快速开始

### 1. 编译并加载内核模块（在 px2-300d 上，需 root）

```sh
cd workspace/lcm_driver
make                 # 或: make KDIR=/path/to/kernel-headers
sh load.sh           # insmod，并自动创建 /dev/lcm 与 /dev/wixbtn
dmesg | tail         # 查看驱动日志
```

加载后出现两个设备节点：

| 设备 | 用途 |
|---|---|
| `/dev/lcm` | 屏幕显示 + 背光控制 |
| `/dev/wixbtn` | Select / Scroll 按键事件 |

常用操作：

```sh
echo 1 > /sys/class/misc/lcm/backlight    # 背光亮（0=灭, 1=亮, 2=闪烁）
insmod wixlcm.ko wixdbg=1                 # 打印 GPIO/背光/按键轮询调试信息
sh unload.sh                              # 卸载（自动息屏）
```

### 2. 通用接口层冒烟自检

```sh
cd workspace/lcm_user
sudo python3 example.py        # 背光/开屏 -> 全亮 -> 全黑 -> 竖条纹 -> 关屏背光灭
```

### 3. 运行 IP / 存储状态屏

编辑 [screen_ip_storage.py](workspace/lcm_user/some_idea/screen_ip_storage.py)
开头的常量后运行：

```python
IP1DEV = "enp1s0"
IP2DEV = "enp2s0"
STORAGE_DEV = {"FS": "ZFS", "PoolName": "zfspool", "ShowName": "zfspool"}
```

```sh
sudo python3 some_idea/screen_ip_storage.py
```

界面（`E1/E2` 标签 + 网卡 IP + up 状态 + ZFS 池 ALLOC/SIZE 实心进度条）。

**节能说明（v1.2）**：背光自动唤醒 + 10 秒自动熄灭已完全由内核驱动
内置管理，状态屏应用不再自行关/开背光。应用只负责每 5 秒采集→渲染→
推送（帧相同则不下发），启动时亮一次背光让用户看到初始画面。

同屏还有一个**功能完全等价的 C 语言版本**（直接 ioctl，无需 Python）：

```sh
cd workspace/lcm_user/some_idea/screen_ip_storage_c
make
sudo ./screen_ip_storage
```

C 版同样在文件开头用宏定义 `IP1DEV / IP2DEV / STORAGE_FS / STORAGE_POOL /
STORAGE_SHOWNAME / ZPOOL_CMD / REFRESH_INTERVAL` 供修改。
（原 `IDLE_OFF_SECONDS` / `BTN_DEVICE` 宏自 v1.2 起已移除——背光节能改由
内核驱动 `auto_off_ms` / `auto_off_on_keypress` 参数控制。）

### 4. TCP JSON 点阵图监听器

[lcm_tcp_listener/](workspace/lcm_user/some_idea/lcm_tcp_listener/) 是一个跑在
px2-300d 上的 **TCP 服务**：监听指定 IP:端口（默认 `127.0.0.1:6776`），
按行接收 JSON 消息，把其中 base64 编码的 1024 字节点阵图显示到屏幕上。

- **三线程模型**：listener（socket accept + JSON 解析 + base64 解码）、
  display（唯一操作 `/dev/lcm` 的线程，串行所有 ioctl）、
  main 看门狗（每 2 秒检查线程存活，意外退出则重新拉起）。
- **启动即打印 help**：无参数运行或 `-h` 都会输出使用说明并退出。
- **可选参数**：`-b bind_ip`（默认 `127.0.0.1`，可设 `0.0.0.0` 对外）、
  `-p port`（默认 `6776`）。
- **零第三方依赖**：极简 base64 解码器 + 极简扁平 JSON 解析器均为手写。

```sh
cd workspace/lcm_user/some_idea/lcm_tcp_listener
make
sudo ./lcm_tcp_listener -b 0.0.0.0 -p 6776   # 对外监听
sudo ./lcm_tcp_listener                      # 无参数 -> 打印 help
```

JSON 协议（每行一条）：

```json
{"frame":"<base64编码的1024字节点阵>"}     // 显示一帧
{"clear":true}                              // 清屏
{"backlight":0}                             // 只灭背光
{"frame":"...","backlight":1}               // 同时显示 + 开背光
```

完整协议说明、发送示例（nc / Python）和内部线程协作细节见
[lcm_tcp_listener.md](workspace/lcm_user/some_idea/lcm_tcp_listener/lcm_tcp_listener.md)。

## 编程模型

通用接口 [example.py](workspace/lcm_user/example.py) 把"硬件能力"与"业务操作"分开：

```python
from example import FrontPanel

class MyApp(FrontPanel):
    def on_start(self):                 # 启动回调
        self.lcd.backlight_on()
    def on_select(self):                # Select 键按下 -> 做什么
        self.lcd.all_white()
    def on_scroll(self):                # Scroll 键按下 -> 做什么
        self.lcd.stripes()

MyApp().run()                           # Ctrl-C 自动清理
```

也可直接使用底层类 `PX2LCM`（显示 / 背光 / 写显存）和 `WixButtons`
（读键 / 回调监听），详见 example.py 文件头注释与自检代码。

## 内核模块参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `sioaddr` | `0x4E` | Super-I/O 配置口地址 |
| `initialize` | `1` | 加载时初始化面板（ST7565 上电命令序列） |
| `initial_backlight` | `0` | 加载时背光状态：0=灭 / 1=亮 / 2=闪烁 |
| `enable_buttons` | `1` | 注册 `/dev/wixbtn` 按键设备；**v1.2 自动背光也依赖此参数**——按键检测定时器只在 `enable_buttons=1` 时启动，设为 0 会同时关闭自动唤醒 |
| `auto_off_ms` | `10000` | 最后一次按键后背光自动熄灭的毫秒数（0 禁用自动熄灭） |
| `auto_off_on_keypress` | `1` | 任意物理按键自动点亮背光并重置熄灭计时；**需要 `enable_buttons=1`** 才有按键检测 |
| `wixdbg` | `0` | 打印详细寄存器 / GPIO / 背光 / 按键轮询调试信息 |

## 技术细节与原始资料

逆向过程、原厂驱动结构、硬件寄存器链路、ioctl 编号、zpool/接口采集方式等
完整技术细节，统一记录在：

**[readfile/work.md](readfile/work.md)**

其中包括：Super-I/O 引脚与逻辑设备映射、LCM 初始化命令序列、
AUXFANOUT 背光寄存器（含"必须先配置引脚复用否则只能调暗不能关灭"的实测结论）、
PCH GPIO→GPE 按键链路，以及如何从官方补丁 `readfile/b2a.patch`
重新提取原厂 3.2 内核源码。

## 环境要求

- 硬件：LenovoEMC px2-300d（Intel Atom D525 + Nuvoton NCT6776F）
- 内核：启用 `CONFIG_MODULES`；背光 class 与按键需 `CONFIG_BACKLIGHT_CLASS_DEVICE`、
  `CONFIG_ACPI`、`CONFIG_PCI`（Debian/Ubuntu 默认开启）
- 构建：对应内核的头文件（`linux-headers-$(uname -r)`）与 gcc
- 用户态：Python 3（状态屏的接口查询与 zpool 调用针对 Debian 13）

## 许可证

内核模块基于 Wistron Corp. 原厂 GPL 驱动改写，采用 **GPL-2.0**；
Python 代码可按 GPL-2.0 使用。原厂补丁 `readfile/b2a.patch` 的版权归其原作者所有。

## 免责声明

驱动会直接读写主板 Super-I/O 的 I/O 端口。代码已加入芯片 ID 校验（非
NCT6776F 拒绝加载），但在真实硬件上加载仍请自担风险，建议先以
`wixdbg=1` 观察日志确认引脚行为。
