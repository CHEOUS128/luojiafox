# luojiafox

**珞珈狐战队** RoboMaster RM26 赛季软件集合。

> 给**第一次拿到这个仓库的人**。看完这份就能知道：有什么、怎么跑起来、现在到哪了、下一步查什么。
> 不需要背景知识；需要动手的地方都给了具体命令。

---

## 〇、一句话现状（2026-10-25）

工业相机画面**已经能在自定义客户端里显示**，链路端到端打通。

| 部分 | 跑在哪 | 状态 |
|---|---|---|
| `autoaim/` | 车上小电脑（Jetson Orin） | ✅ 开机自启，自瞄 + 第二视角编码发送 |
| `lower-machine/` | 车上 STM32（C 板，云台板 + 底板） | ✅ 已烧录，接收并转发 0x0310 |
| `luojiamaster/` | 操作电脑（Windows） | ✅ 已编译，显示官方 UDP + 本车相机画面 + 键鼠控制 |

**画面水平**：平均约 2 帧/秒、有 0.3~2.4 秒的空档，**卡但能辨认**，小窗不再闪断。
规则允许的上限（50Hz × 300B）比我们用的高得多，后面还有很大优化空间——
但**先把画面拿到**这个目标已经完成。

> 这一版相对上一版的实质改动只有三处：Jetson 发送端改单包模式；电控加视频接收转发；
> 客户端加自定义流显示与登录卡死修复。控制、官方图传、比赛数据全程未受影响。

---

## 一、目录（只有这三块 + 这份 README）

```
luojiafox/
├── README.md                 ← 你正在看的
├── autoaim/                  自瞄 + 第二视角发送端（Jetson）
│   ├── README.md             ← 上手读这份
│   ├── my_lf_INF_20261025/   ← 当前运行版本（代码 + configs/standard3.yaml）
│   ├── my_lf_INF_20261013/   ← 上一版，回滚用，不要改
│   └── 档案/                 ← 探索过程（为什么这么改、哪些结论被推翻了）
├── lower-machine/            下位机固件（Keil，两个工程）
│   ├── README.md             ← 上手读这份（协议、接口、编译）
│   ├── 26赛季全向轮步兵/
│   │   ├── gimbal_project/   ← 云台板：云台/发射/IMU/USB CDC/视频转发
│   │   └── chassis_project/  ← 底板：底盘/功率/电容
│   ├── 诊断工具/             ← PC 端串口/MQTT 工具（.ps1/.bat，不需 Python）
│   └── 档案/                 ← 探索过程（逐跳排查、代码核对、被推翻的结论）
└── luojiamaster/             自定义客户端（Windows）
    ├── 使用说明.md           ← 最短路径：怎么跑起来
    ├── 完整复现指南.md       ← 从零编译、相对 ACE 原版的全部改动
    ├── 局域网联调手册.md     ← 本地模拟比赛的接线与 IP
    ├── 源码/                 ← 完整可编译工程
    ├── 程序/                 ← luojiamaster.exe（现成能跑）
    └── 档案/                 ← 探索过程（阶段 2~5、被推翻的结论）
```

**规则**：根目录不放散文档；每个子系统根目录只放 1~3 份「上手要读的」，
剩下全部进各自的 `档案/`。历史结论被推翻时，**改档案里的「已被推翻的结论」表**，不要删文档。

---

## 二、三分钟复现

### 2.1 车上（Jetson）

```bash
# 代码目录（本机已有，不用从 GitHub 拉）
cd /home/nvidia4/my_lf_INF_20261025

# 服务状态（开机自启，跑的是 build/standard_mpc configs/standard3.yaml）
systemctl status autoaim-20261025 --no-pager | head -8

# 改完配置后重启
sudo systemctl restart autoaim-20261025

# 看发送端是否在出帧（grep -a 必须加：这个日志里有二进制字节）
grep -a "VideoSender" logs/autostart.log | tail -n 6
```

判读：`成帧 N 超限丢弃 M`，5 秒内 N 涨约 25（5fps）、M 涨 0~2 为正常。
`[看门狗]` 出现说明卡在相机/云台串口/检测，与图传无关。

### 2.2 车上（STM32 刷固件）

```
Keil 打开 lower-machine/26赛季全向轮步兵/gimbal_project/MDK-ARM/standard_robot.uvprojx
编译（F7）→ 烧录（F8）
```

或者双击 `MDK-ARM/编译并烧录.bat`（需 Keil 在 `C:\Keil_v5`、J-Link 已接）。
成功标志：**`0 Error(s)` 且出现 `Programming Done`**，只有前者=没烧进去。

> 当前车上的固件与仓库 `main` 这一份**逐字节一致**，不需要重烧。

### 2.3 操作电脑（Windows 客户端）

```bat
:: 直接跑现成的
cd /d "<仓库>\luojiamaster\程序"
luojiamaster.exe

:: 或从源码重编（第一次要几分钟）
cd /d "<仓库>\luojiamaster\源码"
一键配置与编译.bat
copy /y build\release\luojiamaster.exe ..\程序\luojiamaster.exe
```

运行前置条件（缺一不可）见 [`luojiamaster/使用说明.md`](luojiamaster/使用说明.md)：
官方赛事引擎已启动 → 官方选手端 **≥ 12.0.0.213** 已登录且画面正常 →
按 `P` 把键鼠控制来源选为「自定义客户端」→ 客户端登录时填同一个机器人 ID。

---

## 三、全链路：一帧画面从相机到屏幕

```
 车上（Jetson + STM32 + VT03）                        操作间（VT13 + 选手端 + 客户端）
 ┌────────────────────────────────────┐              ┌──────────────────────────────────┐
 │ ①相机 → ②编码 → ③USB → ④STM32校验  │              │ ⑦VT13 → ⑧选手端 → ⑨MQTT → ⑩客户端 │
 │                          ↓ ⑤UART    │  ⑥无线       │                                  │
 │                        VT03         │ ◄──────────► │                                  │
 └────────────────────────────────────┘              └──────────────────────────────────┘
```

| 跳 | 内容 | 实现位置 | 状态 |
|---|---|---|---|
| ① | 大恒相机 → Jetson | `autoaim/.../io/camera` | ✅ |
| ② | 中心裁 320 → 缩 160×160 → H.264 全 I 帧，整帧 ≤296B | `io/video_sender/video_sender.cpp` | ✅ 5fps 稳定出帧 |
| ③ | USB CDC，与 32 字节控制帧共用一条线 | `src/standard_mpc.cpp` + `io/gimbal` | ✅ 短写已修（10-24） |
| ④ | 按帧头 `0xFF`/`0xA5` 互斥分流 → CRC8/CRC16 → 8 帧队列 | `gimbal_project/application/Self_aim.c`、`video_forward.c` | ✅ |
| ⑤ | USART6 / **PG14** / 921600 8N1 发给 VT03 | `video_forward.c` | ✅ `HAL_UART_Transmit` 返回 OK |
| ⑥ | VT03 → 无线 → VT13 | 厂商硬件 | ⚠️ **通，但平均只放过约 2 帧/秒（唯一未明的一跳）** |
| ⑦⑧⑨ | 选手端发布 MQTT `CustomByteBlock`（QoS0） | 选手端 | ✅ 抓包与客户端计数一致 |
| ⑩ | 客户端解包 → FFmpeg 解码 → 纹理显示 | `src/video/custom_byte_block_video_decoder.cpp` | ✅ |

### 关键数字（都来自实测，不是估算）

| 项 | 值 |
|---|---|
| 单帧硬上限 | **296 B**（= 300 − 2B 内层长度 − 2B 内层序号），超了整帧丢，**绝不拆包** |
| 外层帧长 | 309 B = 7B 头 + 300B data + 2B CRC16 |
| 发送 | 5 帧/秒，40 包/秒节拍（令牌桶），160×160，约 10 kbps |
| 到达客户端 | 平均 **2.05 帧/秒**，间隔中位 359 ms，p90 1024 ms，最长 2.44 s |
| 内层序号 | 65 秒跨度 306，**没有断号** → 到达的帧是连续的，丢的是成段的 |
| 官方 UDP | 同期稳定 49~69 包/秒，与自定义流**无关** |

### 这条通道怎么用

协议允许 50Hz × 300B ≈ 15 KB/s，我们只用了约 1.5 KB/s。
所以「卡」不是协议限制，是**链路实际只放过约 2 帧/秒**。要变流畅只能把那一跳的丢包查清
（见下一节），或者在同样 2 帧/秒下提高单帧信息量（分辨率/画质还能往上走）。

---

## 四、现在已知的边界与坑

1. **别按 CTRL。** 固件把「上一轮和本轮都按着 CTRL」当复位信号，经 CAN `0x04` 第 4 字节
   让底板执行 `System_Reset()`——整车硬重启，云台不再跟随、粘着底盘，图传和选手端一起重来，
   **必须重开客户端**。切输入法用 Ctrl+Space 也会踩到。
   我们要的修法（改成按住 500ms 才触发）在电控那边编译报错，**已还原、不烧录**，现场就靠避开。
2. **画面空档 1~2 秒正常**。客户端小窗判活是 3 秒（`kCustomVideoLiveTimeout`），
   只影响「显示保持」，不碰接收/解码/渲染。
3. **主画面会自动切**。官方 UDP 一断就临时让给自定义流，UDP 恢复再切回来；按 `H` 改的是「偏好」。
4. **`H` 切主画面、`I` 翻转官方画面**。
5. **小电脑上的 yaml 改完不用重编**，`systemctl restart` 即可；改 C++ 才要重编。
6. **不要跑自瞄的全量 `make`**：`fire_test.cpp` 是遗留问题会编译失败，只编 `standard_mpc` 和 `video_sender_test`。

---

## 五、下一步（按收益排序）

2. **测 ⑥ 这一跳要再烧一次固件。** 固件里已有现成的 `volatile VideoForwardDebug_t VideoForward_Debug`
   （`video_forward.c`），**从未烧到车上过**。注意：上一轮改动（CTRL 500ms）在电控那边编译报错，
   所以这一步要先确认本地能干净编过再谈。之后在 Keil Watch 展开，跑 10 秒读
   `frames`（USB 收到的 309 帧）与 `forwarded`（真写进 USART6 的帧）：
   - 两个一起涨、差距小 → 丢在 VT03 之后；
   - `forwarded` 远小于 `frames` → 丢在板子转发（再看 `overflow` / `tx_error` / `tx_stuck`）。
   这是唯一能把「板子 vs 图传」分开的测量。
2. **画面上调。** 通道只用到 1/10 的带宽，稳定后再把 160×160 往上试 192/240，
   用「到达率」而不是「看起来清楚」当判据。
3. **控制延迟。** 键鼠走 `CustomControl`（约 9~10 Hz），比官方通道慢，是下一个体验瓶颈。

---

## 六、文档地图

| 想干什么 | 看哪 |
|---|---|
| 跑起来客户端 | [`luojiamaster/使用说明.md`](luojiamaster/使用说明.md) |
| 从零编译客户端 / 相对原版改了什么 | [`luojiamaster/完整复现指南.md`](luojiamaster/完整复现指南.md) |
| 本地模拟比赛、接线与 IP | [`luojiamaster/局域网联调手册.md`](luojiamaster/局域网联调手册.md) |
| 部署/编译自瞄、图传参数 | [`autoaim/README.md`](autoaim/README.md) |
| 固件协议、接口、编译烧录 | [`lower-machine/README.md`](lower-machine/README.md) |
| 串口/MQTT 诊断工具怎么用 | [`lower-machine/诊断工具/`](lower-machine/诊断工具/) |
| 过去踩过的坑、被推翻的结论 | 各子系统的 `档案/README.md` |

---

## 七、仓库卫生规则

- 编译产物、日志、IDE 配置**不进仓库**（`.gitignore` 已覆盖；`boost` 因单文件超
  GitHub 100MB 上限也不进，首次编译由 CMake 自动下载）。
- 抓包 `.bin` 只保留 `lower-machine/诊断工具/` 里那 4 个小样本（合计数十 KB，供 `rm0310.py` 复算）；现场新抓的大文件不要入库。
- `程序/luojiamaster.exe` 是**唯一入库的二进制**，方便没编译环境的人直接用；重编后要同步覆盖。
- 新增一份「探索结论」文档时：先在对应子系统根目录放，**结题时移进 `档案/`**，
  并在 `档案/README.md` 时间线顶部加一行。
- 拿不准的东西写「可能性 / 待验证」，不要写成结论；结论被推翻时在档案里**追加**更正表。

---

## 许可与来源

自瞄部分基于[同济大学 SuperPower 战队 25 赛季自瞄算法](https://github.com/TongjiSuperPower)二次开发；
客户端基于 ACE 原版二次开发。均沿用原项目 LICENSE，仅用于技术交流。
