# px2-300d 前面板显示屏操作指南（C / Python）

> 本文档面向 AI 与开发者，描述**如何用 C 或 Python 操作 px2-300d 前面板的
> 128×64 单色显示屏、背光和两个按键**。读完本文即可编写任意新的屏幕应用，
> 不需要改动内核驱动，也不需要先读驱动源码。
>
> **开发组织方式**：`some_idea/` 目录保存现有的两份参考实现（Python 版与 C 版
> 状态屏，功能等价）。后续所有新应用代码写在
> `workspace/lcm_user/some_idea/` 目录下，开发时以本文档为硬件操作依据，
> 需要绘制函数、字库或工程模板时直接参考 / 复制 `some_idea/` 下的对应实现。

---

## 1. 硬件与软件环境

- 屏幕：128×64 像素单色 LCM（ST7565 兼容控制器），挂在主板 Nuvoton
  NCT6776F 超级 I/O 上，软件模拟 8080 并口；背光由 NCT6776F 的 AUXFANOUT
  PWM 引脚驱动。
- 两个前面板按键：**Select（选择/确认）** 和 **Scroll（滚动/翻页）**，
  接 PCH GPIO，由内核 20ms 定时器轮询电平并软件去抖，**按下沿上报一次**。
- 内核模块 `wixlcm.ko` 加载后出现两个字符设备（操作它们需要 **root**）：

| 设备节点 | 用途 |
|---|---|
| `/dev/lcm` | 屏幕显存读写、显示开关、背光控制 |
| `/dev/wixbtn` | Select / Scroll 按键事件 |

模块加载（在 px2-300d 机器上）：

```sh
cd workspace/lcm_driver
make && sh load.sh        # 编译并 insmod，自动创建 /dev/lcm、/dev/wixbtn
sh unload.sh              # 卸载（自动息屏、关背光）
```

背光另外提供两种免代码控制方式：

```sh
# sysfs（/sys/class/misc/lcm/backlight，值：0=灭 1=亮 2=硬件闪烁）
echo 1 | sudo tee /sys/class/misc/lcm/backlight
cat /sys/class/misc/lcm/backlight

# 标准 backlight class（内核启用时存在）
/sys/class/backlight/wixlcm/brightness
```

---

## 2. 显存模型（最重要的一节）

屏幕 **128 列 × 64 行**，显存按 **8 个 page（页）× 128 列** 组织，共
**1024 字节**：

- page 0 对应像素行 0~7，page 1 对应行 8~15，……，page 7 对应行 56~63；
- 每个字节驱动**一列中纵向的 8 个点**：bit0 = 该列最上面的点，
  bit7 = 最下面的点；位置 1 = 点亮，0 = 熄灭；
- 像素坐标 `(x, y)`（x: 0~127 从左到右，y: 0~63 从上到下）在整帧
  1024 字节缓存中的位置：

```text
index = (y >> 3) * 128 + x        # page = y/8, column = x
bit   = y & 7                     # 字节内位号
frame[index] |= (1 << bit)        # 点亮该像素
frame[index] &= ~(1 << bit)       # 熄灭该像素
```

- 一帧 = 1024 字节，按 page 分 8 次、每次 128 字节下发；
- 也支持**页内局部写**：指定起始 column 和长度 size（≤128，
  且 `column + size ≤ 128`），只刷新一段连续列，控制器 GRAM 中
  未写到的内容保持不变；
- "显示开关"（控制器 0xAF/0xAE，决定面板是否扫描显示 GRAM）与
  "背光"（LED 亮/灭/闪）是两个独立控制，可分别操作。

---

## 3. ioctl ABI 一览（C 与 Python 底层完全相同）

权威定义见 [workspace/lcm_driver/lcm_uapi.h](../../lcm_driver/lcm_uapi.h)
（C 目录下还有一份可直接复制的副本）。magic 为 `0xDD`。

### 3.1 `/dev/lcm`：屏幕 + 背光

| ioctl | 编码 | 参数 | 作用 |
|---|---|---|---|
| `IOCTL_DISPLAY_COMMAND` | `_IO(0xDD, 1)` | `struct lcm_info *` | 主命令，具体动作由 `ctrl` 字段选择 |
| `WIX_LCM_IOC_PON` | `_IO(0xDD, 7)` | 无 | 显示开（控制器 0xAF） |
| `WIX_LCM_IOC_POFF | `_IO(0xDD, 8)` | 无 | 显示关（控制器 0xAE） |
| `WIX_LCM_IOC_BL_SET` | `_IOW(0xDD, 10, int)` | `int *`（0/1/2） | 设置背光：灭 / 亮 / 闪烁 |
| `WIX_LCM_IOC_BL_GET` | `_IOR(0xDD, 11, int)` | `int *` | 读回当前背光状态 |

`IOCTL_DISPLAY_COMMAND` 的参数结构（共 144 字节：4 个 int + 128 字节数据）：

```c
#define WIX_LCM_CMD_PON    7    /* ctrl=7：显示开 */
#define WIX_LCM_CMD_POFF   8    /* ctrl=8：显示关 */
#define WIX_LCM_CMD_RESET  9    /* ctrl=9：设置 page/column 地址并写入显示数据 */

struct lcm_info {
    int  ctrl;                  /* WIX_LCM_CMD_* */
    int  page;                  /* 页号 0..7 */
    int  column;                /* 起始列 0..127 */
    int  size;                  /* data 中有效字节数，<=128，column+size<=128 */
    char data[128];             /* 列数据，每字节 8 个纵向点 */
};
```

背光取值：`WIX_LCM_BL_OFF=0`、`WIX_LCM_BL_ON=1`、`WIX_LCM_BL_BLINK=2`
（闪烁是硬件 PWM 行为，不需要软件反复切换）。

### 3.2 `/dev/wixbtn`：两个按键

| ioctl | 编码 | 参数 | 作用 |
|---|---|---|---|
| `READ_BUTTON_NONBLOCKING` | `_IOWR('c', 150, int)` | `int *` | 非阻塞读一个键，无键返回 0 |
| `READ_BUTTON_BLOCKING` | `_IOWR('c', 151, int)` | `int *` | 阻塞读到一个键为止 |

- 按键码：`4 = WIX_BTN_LCM_SELECT`，`5 = WIX_BTN_LCM_SCROLL`，
  `0 = WIX_NO_BTN`（无键）。
- 除 ioctl 外，直接 `read(fd, &code, 4)` 也可读到 **4 字节小端 int** 按键码：
  默认阻塞；`open` 时加 `O_NONBLOCK` 则无事件返回 `-EAGAIN`。
- 设备支持 **`poll`/`select`/`epoll`**（`EPOLLIN`），可以在主循环里
  "等按键或等超时"，这是状态屏应用最常用的模式。

---

## 4. Python 怎么操作屏幕

环境：Python 3（代码兼容 2.7），**仅标准库**（`os/fcntl/struct/threading/
select/array`），以 root 运行，模块已加载。

### 4.1 通用接口层 `example.py`

[workspace/lcm_user/example.py](../example.py) 是工程中唯一直接操作
`/dev/lcm`、`/dev/wixbtn` 的通用模块，提供三个类，新程序 `import` 即可：

```python
import os, sys
# 从 some_idea/<app>/ 下定位到 lcm_user 目录
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from example import PX2LCM, WixButtons, FrontPanel, LCMError
```

### 4.2 `PX2LCM`：屏幕与背光原语（只负责"把数据送进硬件"）

| 方法 | 作用 |
|---|---|
| `PX2LCM(device="/dev/lcm")` / `close()` | 打开/关闭设备；支持 `with` 语法 |
| `display_on()` / `display_off()` | 显示开 / 关（0xAF / 0xAE） |
| `set_backlight(state)` / `get_backlight()` | state：0 灭 / 1 亮 / 2 闪 |
| `backlight_on()` / `backlight_off()` / `backlight_blink()` | 背光三态快捷方法 |
| `write_page(page, data, column=0)` | 写一个页（8 像素高条带），`data` 为 bytes，最长 128 |
| `write_frame(frame)` | 写整屏，`frame` 必须为 **1024 字节**（内部自动分 8 页） |
| `fill(value=0x00)` | 整屏填充同一字节（`0x00` 全黑 / `0xFF` 全亮 / `0xAA` 隔点） |
| `clear()` / `all_white()` / `all_black()` / `stripes()` | 清屏 / 全亮 / 全黑 / 竖条纹测试画面 |

类常量：`PX2LCM.WIDTH == 128`、`HEIGHT == 64`、`PAGES == 8`。

### 4.3 `WixButtons`：按键读取

| 用法 | API |
|---|---|
| 主动读 | `read(blocking=True)` 返回按键码（0/4/5）；`read_name()` 返回 `"NONE"/"SELECT"/"SCROLL"` |
| 超时等待 | `wait_event(timeout=None)`：内部用 `select`，返回按键码或 `None` |
| 回调监听 | 给 `callback` 赋一个 `fn(code)` 函数后 `start()`，后台守护线程按下即回调；`stop()` 停止 |

打开失败（驱动未启用按键）会抛 `LCMError`，应用可捕获后降级为"无按键"运行。

### 4.4 `FrontPanel`：应用基类（按键 → 动作的分派框架）

继承后重写回调钩子即可，`run()` 阻塞运行并在 Ctrl-C 时自动清理设备：

```python
class MyApp(FrontPanel):
    def on_start(self):                 # 启动后调用一次：开背光、画初始画面
        self.lcd.backlight_on()
    def on_select(self):                # Select 键（4）
        self.lcd.all_white()
    def on_scroll(self):                # Scroll 键（5）
        self.lcd.stripes()
    def on_stop(self):                  # 退出前清理
        pass

MyApp().run()
```

也可重写统一入口 `on_button(code)` 自行分派。

### 4.5 在 Python 里画任意内容（帧缓存画法）

通用模式：**自己维护一个 1024 字节 `bytearray` 帧缓存 → 在缓存上画像素 /
文字 / 图形 → `lcd.write_frame(bytes(fb))` 整屏下发**。

[screen_ip_storage.py](screen_ip_storage/screen_ip_storage.py) 里有一套
完整、可直接参考或复制的绘制函数与字库：

- `FONT_5X7`：内置 **5×7 列主序 ASCII 点阵字库**，每字 5 字节（每字节一列，
  bit0 为最上一行），覆盖 0-9、A-Z、a-z 和常用标点；未收录字符回退大写、
  再回退空格。需要新字符时按同样格式往字典里加 5 个字模即可。
- `fb_new()`：新建全黑帧（1024 字节）。
- `fb_set_pixel(fb, x, y, on=True)`：点 / 灭一个像素，自动裁剪越界坐标。
- `fb_draw_char(fb, x, y, ch)` / `fb_draw_text(fb, x, y, text)`：5×7 字符
  与字符串，字符步进宽 6 像素（5 字模 + 1 间距），返回绘制结束列。
- `fb_fill_rect(fb, x0, y0, w, h, on=True)`：实心矩形（进度条、反白条等）。

最小完整例子：

```python
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from example import PX2LCM

W, H = 128, 64
fb = bytearray(W * H // 8)             # 1024 字节帧缓存

def pixel(x, y, on=True):
    if 0 <= x < W and 0 <= y < H:
        idx = (y >> 3) * W + x
        fb[idx] = (fb[idx] | (1 << (y & 7))) if on else (fb[idx] & ~(1 << (y & 7)))

with PX2LCM() as lcd:
    lcd.backlight_on()
    lcd.display_on()
    for x in range(W):                 # 画一条对角亮线
        pixel(x, x // 2)
    lcd.write_frame(bytes(fb))         # 整屏一次性下发
```

想绕开 `example.py` 直接发 ioctl 也可以：`struct lcm_info` 用
`struct.pack("iiii", ctrl, page, column, size) + payload(128字节以内)`
组包（总长 144 字节），对 `/dev/lcm` 句柄 `fcntl.ioctl(fd, IOCTL_DISPLAY_COMMAND, buf, 1)`；
所有 ioctl 编号常量在 `example.py` 开头都有定义。但推荐直接 `import example`。

---

## 5. C 怎么操作屏幕

环境：gcc + 内核头文件已装即可编译用户态程序；**不依赖任何第三方库**，
以 root 运行。参考工程见
[screen_ip_storage_c/](screen_ip_storage_c/)（`screen_ip_storage.c`、
`glyph.h`、`lcm_uapi.h`、`Makefile`）。

### 5.1 准备头文件

把 [lcm_uapi.h](../../lcm_driver/lcm_uapi.h)（ABI 定义，C 目录下有副本）
放到自己的源码目录后 `#include "lcm_uapi.h"`，即可得到全部 ioctl 编号、
`struct lcm_info` 和按键码定义。需要字符显示时再复制 `glyph.h`。

### 5.2 打开设备、开显示、开背光

```c
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "lcm_uapi.h"

int fd = open("/dev/lcm", O_RDWR);          /* 失败检查 errno */
ioctl(fd, WIX_LCM_IOC_PON, 0);              /* 显示开 */
int bl = WIX_LCM_BL_ON;                     /* 0 灭 / 1 亮 / 2 闪 */
ioctl(fd, WIX_LCM_IOC_BL_SET, &bl);
```

### 5.3 写显存（整帧分 8 页下发）

```c
uint8_t frame[8 * 128];                     /* 1024 字节帧缓存 */
/* ... 在 frame 上绘制 ... */

for (int page = 0; page < 8; page++) {
    struct lcm_info info;
    memset(&info, 0, sizeof(info));
    info.ctrl   = WIX_LCM_CMD_RESET;
    info.page   = page;
    info.column = 0;
    info.size   = 128;
    memcpy(info.data, frame + page * 128, 128);
    ioctl(fd, IOCTL_DISPLAY_COMMAND, &info);
}
```

局部刷新时设置 `info.column` 为起始列、`info.size` 为实际字节数即可。
显示关 / 背光灭：`ioctl(fd, WIX_LCM_IOC_POFF, 0)`、
`bl = WIX_LCM_BL_OFF` 后再 `BL_SET`。

### 5.4 像素绘制与字库

帧缓存上点像素的写法（与 Python 完全同构）：

```c
static void set_pixel(uint8_t *fb, int x, int y, int on)
{
    if (x < 0 || x >= 128 || y < 0 || y >= 64) return;
    if (on)
        fb[(y >> 3) * 128 + x] |=  (uint8_t)(1u << (y & 7));
    else
        fb[(y >> 3) * 128 + x] &= (uint8_t)~(1u << (y & 7));
}
```

`glyph.h` 提供 5×7 列主序字模表 `FONT[]` 与 `find_glyph(ch)` 查找函数
（未收录字符回退大写再回退空格），配合 `GLYPH_WIDTH=5 / GLYPH_HEIGHT=7`
即可实现 `draw_char / draw_text / fill_rect`，参考实现里的同名函数可
直接复制。加新字符时在 `FONT[]` 表里追加一条 `{'x', {5 个字模}}`。

### 5.5 读按键（阻塞 ioctl / read / select 三种都行）

```c
int bfd = open("/dev/wixbtn", O_RDWR);

/* 方式 A：阻塞 ioctl 读一个键 */
int code = 0;
ioctl(bfd, READ_BUTTON_BLOCKING, &code);    /* code=4 Select / 5 Scroll */

/* 方式 B：普通 read，4 字节本机序 int */
read(bfd, &code, sizeof(code));

/* 方式 C：select 等"按键或超时"（状态屏主循环常用） */
fd_set rfds; struct timeval tv = { .tv_sec = 5 };
FD_ZERO(&rfds); FD_SET(bfd, &rfds);
if (select(bfd + 1, &rfds, NULL, NULL, &tv) > 0)
    read(bfd, &code, sizeof(code));
```

### 5.6 Makefile 要点

```makefile
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11 -D_GNU_SOURCE
app: app.o
	$(CC) $(CFLAGS) -o $@ $^
```

`-D_GNU_SOURCE`（或 `-std=gnu11`）在用到 `struct ifreq / SIOCGIFADDR /
popen` 等 POSIX/GNU 声明时需要；纯屏幕+按键程序不加也能编。编译运行：

```sh
make
sudo ./app
```

---

## 6. 屏幕可以做的事（能力清单）

- **任意点阵内容**：文本、数字、IP / 主机名 / 时间 / 系统状态、小图标、
  几何图形、实心进度条、反白高亮行等——本质都是操作 1024 字节帧缓存。
- **整屏刷新与局部刷新**：`write_frame` 整屏；`write_page` /
  `struct lcm_info.column+size` 页内局部更新，适合动画或只改一行。
- **背光三态**：常亮、熄灭、硬件闪烁；sysfs / backlight class / ioctl
  三种途径任选。
- **按键交互**：阻塞等待、非阻塞轮询、`select`/`poll` 超时多路等待、
  Python 回调线程；每按一下上报一个事件。
- **节能状态屏范式**（参考实现已验证，可复用其思路）：缓存上一帧，
  新帧逐字节相同则不下发；画面连续 N 秒无变化自动关背光；背光灭期间
  显存照常更新，按任意键重新点亮并重置计时。
- **系统信息采集搭配**：Python 用标准库 / subprocess（如
  `zpool list -H -p -o alloc,size`、`SIOCGIFADDR` 取 IP、读
  `/sys/class/net/<if>/operstate`）；C 用 `socket+ioctl`、`popen`、
  sysfs 文件读取，两个参考实现里均有现成代码可复制。
- **多屏 / 多语言**：C 和 Python 可以各自独立运行操作同一设备；字库可按
  5×7 列主序格式自行扩展（含自定义小图形字模）。

---

## 7. 参考文件速查

| 文件 | 内容 |
|---|---|
| [workspace/lcm_driver/lcm_uapi.h](../../lcm_driver/lcm_uapi.h) | ioctl ABI、`struct lcm_info`、按键码的**权威定义** |
| [workspace/lcm_driver/wixlcm.c](../../lcm_driver/wixlcm.c) | 内核驱动源码，想确认设备行为（阻塞/去抖/poll/背光寄存器）时查阅 |
| [workspace/lcm_driver/load.sh](../../lcm_driver/load.sh) / [unload.sh](../../lcm_driver/unload.sh) | 模块加载 / 卸载 |
| [workspace/lcm_user/example.py](../example.py) | Python 通用硬件接口层（`PX2LCM` / `WixButtons` / `FrontPanel`），直接运行可冒烟自检 |
| [screen_ip_storage/screen_ip_storage.py](screen_ip_storage/screen_ip_storage.py) | Python 状态屏参考实现：信息采集、5×7 字库、帧缓存绘制、节能唤醒全套 |
| [screen_ip_storage_c/screen_ip_storage.c](screen_ip_storage_c/screen_ip_storage.c) | 与上功能等价的 C 实现：裸 ioctl、`select` 等键、Makefile 工程 |
| [screen_ip_storage_c/glyph.h](screen_ip_storage_c/glyph.h) | C 版 5×7 ASCII 点阵字库（含 `find_glyph`） |
| [screen_ip_storage_c/lcm_uapi.h](screen_ip_storage_c/lcm_uapi.h) | 可供 C 新应用复制的 ABI 头文件副本 |

## 8. 新应用开发落点（`some_idea/`）

- 新功能代码统一放在 `workspace/lcm_user/some_idea/` 下，Python 或 C
  任选，每个应用可建独立子目录。
- Python 应用：`sys.path` 指向 `lcm_user/` 后 `from example import
  PX2LCM, WixButtons, FrontPanel` 即获得全部硬件原语；绘制函数与字库可从
  `some_idea/screen_ip_storage/` 的参考实现复制改造。
- C 应用：把 `lcm_uapi.h`（需要字库再加 `glyph.h`）复制进自己的目录，
  按第 5 节直接 ioctl；Makefile 可照抄参考工程。
- 硬件操作约定：root 权限运行；运行前 `wixlcm.ko` 已加载；退出前关闭背光、
  显示并关闭设备句柄（Python 的 `FrontPanel.run()` / `with PX2LCM()` 会
  自动处理）。
