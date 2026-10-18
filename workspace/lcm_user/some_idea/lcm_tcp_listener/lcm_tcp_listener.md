# lcm_tcp_listener

一个跑在 px2-300d 上的**TCP JSON 屏幕监听器**：监听指定 IP:端口，接收
一行一条的 JSON 消息，把其中的点阵图数据写入 /dev/lcm 显示到
128×64 单色 LCM 屏幕上。

## 线程模型

程序内部有三个线程：

| 线程 | 职责 |
|---|---|
| **listener** | TCP accept → 读一行 JSON → `mcj_parse` → `base64_decode` → 加锁写入共享帧缓冲、唤醒 condvar |
| **display**  | 等 condvar → 从共享帧缓冲取最新帧 → 通过 `/dev/lcm` 的 `IOCTL_DISPLAY_COMMAND` 分 8 页下发到屏幕 |
| **main**     | 看门狗。每 2 秒检查一次两个工作线程的 `heartbeat` 计数器以及 `pthread_kill(tid, 0)` 存活状态。只要某线程意外退出，就自动 `join` 后 `pthread_create` 拉起来 |

listener 和 display 通过 `pthread_mutex_t` + `pthread_cond_t` 协作，
一帧一帧地流水线工作：即使 display 还在刷上一帧，listener 也已经可以
解析和接收下一帧了（虽然共享缓冲只有 1 份，最新帧会覆盖）。

## 编译与运行

在 px2-300d Debian 主机上（内核模块 `wixlcm.ko` 已加载）：

```sh
cd workspace/lcm_user/some_idea/lcm_tcp_listener
make
sudo ./lcm_tcp_listener                     # 默认 127.0.0.1:6776
sudo ./lcm_tcp_listener -b 0.0.0.0 -p 6776  # 监听所有网卡
sudo ./lcm_tcp_listener -p 9000             # 换个端口
```

按下 Ctrl-C 或 `kill` 后，程序会把 `g_shutdown` 置 1、关闭监听 socket、
broadcast condvar，然后 join 两个工作线程并退出。

## JSON 协议

每条消息**一行一个完整 JSON 对象**，以 `\n`（或 `\r\n`）结尾。
可以一个连接发一条然后断开，也可以在同一 TCP 连接上连续发多条。

### 字段一览

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `frame`      | string  | 否（和 `clear` 二选一） | **base64 编码的 1024 字节**帧缓冲（128×64 像素，8 页 × 128 列的 GRAM 原始格式） |
| `clear`      | boolean | 否 | `true` 表示清屏（下发 1024 字节 0x00），不需要 `frame` |
| `backlight`  | integer | 否 | 直接设置背光：`0` 灭 / `1` 亮 / `2` 硬件闪烁。不传则不变 |

- `frame` 解码后**必须正好是 1024 字节**（也就是 `ceil(1024/3)*4 = 1368` 字符左右的 base64 串）。
- 帧格式和 C 参考实现、Python 参考实现里的 `frame[1024]` 完全一致：
  `index = (y >> 3) * 128 + x`，bit0 = 该列最上面的点，bit7 = 最下面的点，
  1 = 亮，0 = 灭。

### 例子

1. **显示一帧**（base64 里塞任意生成的 1024 字节）：

```json
{"frame":"<1368字符左右的base64...>\n"}
```

2. **清屏**：

```json
{"clear":true}\n
```

3. **只开关背光**：

```json
{"backlight":0}\n
```

### 发送测试（nc / Python）

```sh
# bash: 把本地文件 frame.bin (1024字节) 编成 base64 再发
base64 -w0 frame.bin | (echo -n '{"frame":"'; cat; echo '"}') | nc -q1 127.0.0.1 6776
```

```python
# python3
import socket, base64, json
s = socket.create_connection(("127.0.0.1", 6776))
frame = open("frame.bin","rb").read()   # 1024 bytes
msg = json.dumps({"frame": base64.b64encode(frame).decode()}) + "\n"
s.sendall(msg.encode())
s.close()
```

## 文件

| 文件 | 作用 |
|---|---|
| `lcm_tcp_listener.c` | 主程序，三线程架构，socket/JSON/base64/ioctl 全部串起来 |
| `base64.c` / `base64.h` | 极简 base64 解码器，无第三方依赖 |
| `mincjson.c` / `mincjson.h` | 极简扁平 JSON 对象解析器（只支持字符串 / bool / null / 整数，不支持嵌套对象 / 数组），无第三方依赖 |
| `lcm_uapi.h`          | `/dev/lcm` 的 ioctl ABI 定义（从内核 uapi 头文件复制） |
| `Makefile`            | `make` 即可 |

## 限制 / 注意事项

- **必须 root 运行**，否则 `/dev/lcm` 打不开。
- **单连接串行处理**：listener 一次只服务一个 accept，其它客户端在内核
  listen 队列里排队；如果需要并发可以后续加连接池。
- **帧缓冲只保留最新一帧**：display 线程忙的时候 listener 收到的新帧会
  覆盖上一帧（`g_frame_ready` 位），这是设计意图——LCM 只有 1fps 左右
  的刷新上限，丢帧是合理的。
- **心跳只做"存活性"检查**，不做 watchdog 强制重启：只有当
  `pthread_kill(tid, 0)` 返回 ESRCH（线程真的退出了）才会 respawn；
  心跳值不变只打一条 WARN。display 线程正常情况下大多数时候都在
  `pthread_cond_wait`，所以它的 heartbeat 只有收到新帧才更新，不代表
  "卡死"。
- JSON 解析器只够处理本协议需要的扁平对象；不要用它解析任意 JSON。
