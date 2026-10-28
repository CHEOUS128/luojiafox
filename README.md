# luojiafox

**珞珈狐战队** RoboMaster RM26 赛季软件集合。

> 给**第一次拿到这个仓库的人**（哪怕你换了一台完全不同的车）。
> 看完这份你就知道：这套东西由哪几块组成、需要什么硬件、按什么顺序装、
> 哪些参数是**这台车专属**必须重做、每一步怎么判断自己做对了。
> 不需要背景知识；需要动手的地方都给了具体命令。

---

## 〇、一句话现状（2026-10-28）

工业相机画面**已经能在自定义客户端里显示**，自瞄**已经在实车上自动瞄准**，链路端到端打通。

| 部分 | 跑在哪 | 状态 |
|---|---|---|
| `autoaim/` | 车上小电脑（Jetson Orin） | ✅ 开机自启，自瞄 + 第二视角编码发送 |
| `lower-machine/` | 车上 STM32（C 板，云台板 + 底板） | ✅ 已烧录，接收并转发 0x0310 |
| `luojiamaster/` | 操作电脑（Windows） | ✅ 已编译，显示官方 UDP + 本车相机画面 + 键鼠控制 |

**画质**：192×152 灰度、20 fps 编码、约 60 kbps。静止时能看清装甲板与场地细节，**已到「可接受」**。

**帧率**：客户端实际出画**明显低于 20 fps**。**怀疑是硬件/图传那一跳的通过能力**（VT03→VT13），
不是编码也不是协议——但**这一条尚未证实**，见本文第六节「已知边界与坑」第 1 条。

---

## 一、这套东西是什么

三条**互相独立**的链路，可以分开装、分开验证。这是复现的关键——
不要一上来就整车联调，那样坏了不知道是哪一层。

```
① 自瞄链路     相机 → Jetson(识别/预测/规划) → USB → STM32(云台电机) → 打中
② 官方链路     赛事引擎 → 交换机 → 操作电脑(选手端) → MQTT/UDP → 自定义客户端
③ 第二视角     同一台相机 → Jetson(H.264) → USB → STM32 → VT03 → 无线 → VT13
                → 选手端 → MQTT CustomByteBlock → 自定义客户端(H 键切换)
```

① 和 ② 都**不依赖** ③。所以 ① 没调好也可以先把 ② 跑起来，③ 没通也不影响自瞄。
**顺序建议：② → ③ → ①**。理由：② 与车无关（一台电脑就能验），③ 能给你一双眼睛
（装 ① 的时候能看到相机画面），① 最难也最耗时。

---

## 二、目录（只有这三块 + 这份 README）

```
luojiafox/
├── README.md                 ← 你正在看的
├── autoaim/                  自瞄 + 第二视角发送端（Jetson）
│   ├── README.md             ← 上手读这份（编译、部署、参数）
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

## 三、换一台车怎么复现

### 3.0 先看清楚：你手上必须有什么

| 层 | 需要 | 没有会怎样 |
|---|---|---|
| 机器人本体 | RoboMaster 26 赛季步兵（或同代下位机） | 下面全部作废 |
| 小电脑 | Jetson Orin（Ubuntu 22.04）；Nano/NX 需重编 CUDA 部分 | 自瞄跑不动 |
| 相机 | 工业相机（大恒 / 海康 / 迈德威视，代码三家都支持） | 自瞄没输入 |
| 云台板 | STM32F407 开发板 C 型 + Keil | 控制帧没人收 |
| 图传 | 官方 VT03（发送）+ VT13（接收） | ③ 链路无法打通 |
| 操作电脑 | Windows + 官方选手端 **≥ 12.0.0.213** | 客户端登录不了/没有 MQTT |

> **选手端版本是硬门槛。** 低于 12.0.0.213 的版本不给自定义客户端转发 MQTT
> `CustomByteBlock`，画面永远出不来。这一条排查时会浪费你一天，先说。

### 3.1 阶段一：客户端先跑起来（不需要车）

这是唯一**完全不依赖车**的一步，先把它做完。

```bat
:: 直接跑现成的
cd /d "<仓库>\luojiamaster\程序"
luojiamaster.exe

:: 或从源码重编（第一次要几分钟）
cd /d "<仓库>\luojiamaster\源码"
一键配置与编译.bat
copy /y build\release\luojiamaster.exe ..\程序\luojiamaster.exe
```

前置条件（缺一不可）：赛事引擎已启动 → 选手端 ≥12.0.0.213 已登录且官方画面正常
→ 按 `P` 把键鼠控制来源选为「自定义客户端」→ 客户端登录时填同一个机器人 ID。

**验收**：能登录、能看到官方图传、按键有响应。
详细步骤和全部坑见 [`luojiamaster/使用说明.md`](luojiamaster/使用说明.md) 与
[`完整复现指南.md`](luojiamaster/完整复现指南.md)。

### 3.2 阶段二：固件烧进去

```
Keil 打开 lower-machine/26赛季全向轮步兵/gimbal_project/MDK-ARM/standard_robot.uvprojx
编译（F7）→ 烧录（F8）
```

或者双击 `MDK-ARM/编译并烧录.bat`（需 Keil 在 `C:\Keil_v5`、J-Link 已接）。
**成功标志：`0 Error(s)` 且出现 `Programming Done`**——只有前者=没烧进去。

> **换车时这一层不是复制粘贴就完事。** 固件里有三样东西是跟车绑死的：
> 定时器与 GPIO 分配（VT03 接在哪个串口）、CAN ID 与电机方向、
> 以及云台/底盘的机械参数。见第四节「换车必须重做的参数」。

**验收**：串口能看到 32 字节控制帧被正确解析（`Self_aim.c` 里的 `RECEIVE_DATA_SIZE = 32`），
云台跟随自瞄程序发的角度。**先不做图传**，把 `video_send_enable: false` 关掉再验，
避免 309 字节视频帧混进来干扰判断。

### 3.3 阶段三：自瞄本体 + 标定（最耗时）

**这一步没有捷径，也不能抄我们的参数。** 标定结果跟相机、镜头、云台、
装车位置**逐台不同**。

#### 3.3.1 编译

```bash
cd autoaim/my_lf_INF_20261025
mkdir -p build && cd build
cmake ..
make standard_mpc video_sender_test -j$(nproc)
```

> ⚠️ **不要跑全量 `make`**：`fire_test.cpp` 是项目遗留问题，会编译失败。
> 生产目标是 `standard_mpc`。

依赖：OpenCV 4、fmt、Eigen3、spdlog、yaml-cpp、nlohmann_json、CUDA、TensorRT 或 OpenVINO，
以及图像传的 `libavcodec-dev libswscale-dev libavutil-dev libx264-dev`，
再加相机厂商 SDK（大恒 / 海康 / 迈德威视三选一，按你手上的相机装）。

#### 3.3.2 模型文件（**不在仓库里，必须自己准备**）

`.gitignore` 排除了 `autoaim/**/assets/`（136 MB，含 62 MB 演示视频）。你要自己补齐：

| 文件 | 用途 | `standard3.yaml` 里哪个键 |
|---|---|---|
| `assets/yolov5_fp32.engine` | 装甲板识别（TensorRT，**当前实际在用**） | `yolov5_model_path` |
| `assets/yolo11.xml` | buff 识别（OpenVINO） | `yolo11_model_path` |
| `assets/yolo11_buff_int8.xml` | buff 识别 int8 版 | `model` |
| `assets/yolov8.xml` | 装甲板识别（OpenVINO 备选） | `yolov8_model_path` |
| `assets/tiny_resnet.onnx` | 数字/颜色分类 | `classify_model` |

**TensorRT engine 不能跨机器复制**——它是按显卡架构和 TensorRT 版本编译的，
在别人机器上跑的 engine 拷到你这儿会加载失败。必须用你自己机器的
`trtexec --onnx=xxx.onnx --saveEngine=assets/yolov5_fp32.engine` 重新生成。
同一台机器换 TensorRT 版本也要重生成。

> ⚠️ **engine 是硬性前置，绕不过去。** `YOLOV5` 的构造函数在检查 `use_traditional`
> **之前**就无条件打开 `yolov5_model_path` 指向的文件，找不到直接
> `throw runtime_error("YOLOV5: cannot open engine file: ...")` 退出。
> 所以 `use_traditional: true` **不能**用来「先不装模型跑一下」——程序照样起不来。
>
> 想单独验证相机和编码通不通，用 **`video_sender_test`**：它只依赖相机和编码器，
> 不碰 YOLO、不碰 CAN、不碰云台串口，是这套代码里最干净的「相机是否出图」体检：
> ```bash
> sudo ./build/video_sender_test /tmp -c configs/standard3.yaml --compare
> # 看一眼 /tmp/video_test_view.png 有没有画面；不接相机也能跑（合成图模式，默认）
> ```
> `calibration/capture` 不行——它同时构造 `io::CBoard`，没接 C 板/CAN 会直接失败。
> 相机出图了再回头处理模型，这样能把「相机坏」和「模型没配好」分开。

#### 3.3.3 标定（换车必做）

这是 ① 链路唯一没法抄的部分。按顺序做，每步都有对应的可执行程序：

| # | 标定什么 | 程序 | 结果写到哪 | 不做的后果 |
|---|---|---|---|---|
| 1 | **相机内参** | `./build/calibrate_camera` | `configs/standard3.yaml` 的 `camera_matrix` / `distort_coeffs` | 距离和角度全错，怎么调都打不中 |
| 2 | **手眼标定**（相机→云台） | `./build/calibrate_handeye` | `R_camera2gimbal` / `t_camera2gimbal` | 近距离准、远处偏，转身就飘 |
| 3 | **云台→IMU 旋转** | 手工填 | `R_gimbal2imubody` | 姿态解算错，pitch 补偿反向 |
| 4 | **弹道偏移** | 打靶实测 | `yaw_offset` / `pitch_offset` | 准星对得上但弹着点偏一个恒定角度 |
| 5 | **弹速** | 实测后改源码重编 | 编译期常量 | 提前量全错 |

标定程序的用法（三个都用同一套命令行约定，输入图默认 `assets/img_with_q/`）：

```bash
cd autoaim/my_lf_INF_20261025
./build/capture_gimbal  -c configs/calibration.yaml           # ① 采集：按 s 存图，按 q 退出
./build/calibrate_camera assets/img_with_q -c configs/calibration.yaml   # ② 内参
./build/calibrate_handeye assets/img_with_q -c configs/calibration.yaml  # ③ 手眼
```

标定板是**棋盘格**，格数与方格尺寸在 `configs/calibration.yaml`（默认 8×6、29 mm）。

> ⚠️ **换板子要改两个地方。** `calibration/capture_gimbal.cpp:126` 把内角点数
> **硬编码**成 `cv::Size patternSize(5, 7)`，**不读 yaml**；`calibrate_camera`
> 才从 yaml 按「格数 − 1」推。两边是转置关系，格数一致时能容忍，
> **格数一改采集端就永远识别失败**。换板子记得改这一行并重编 `capture_gimbal`。
>
> 另外 `calibration/capture` 用的是**圆点阵列**（10×7），不是棋盘格，别混用。
> 完整步骤和每步的判据见 [`autoaim/README.md`](autoaim/README.md) 的「换车必须重做的标定」。

> **弹速也要改。** 代码里写死 27.0 m/s，planner 钳到 22。
> 你的发射机构不一样就一定要改，否则提前量全错。这一项是编译期常量（已知欠债 W1）。

#### 3.3.4 验收

在 `configs/standard3.yaml` 里先把 `auto_fire: false`（只跟不打），

```bash
./build/standard_mpc configs/standard3.yaml
```

**验收标准**：把装甲板放在 2~3 米处匀速横移，
云台能**平稳跟住不抖、不甩**，日志里有 `[DualModel]` 输出。
确认跟得住，再把 `auto_fire` 改回 `true` 去打靶。

### 3.4 阶段四：第二视角图传（③ 链路）

前面三步都通了再做这个。它的作用是**多一双眼睛**，不影响自瞄本身。

#### 3.4.1 确认转发串口

固件把 VT03 接在 **USART6 / PG14 / 921600 8N1**（见 `Src/usart.c`）。
**换板子必须重新确认这一条**——如果你的 C 板 USART6 不是引到 PG14、
或者 VT03 根本没接在这个口上，图传一定不通，而且现象和「VT03 坏了」一模一样。

> ⚠️ **接线：板子 3pin 上那根 12V 千万别接。**
> 板子侧只需要 **GND + TXD（2 号脚）**，**3 号脚（12V）和 5V / VCC / 3V3 都不要碰**。
> VT03 用它自己那路 12V 供电——**不要**从 USB-TTL 的 5V 取电（电流不够，而且电压不对）。
> 接线细节见 [`lower-machine/档案/第二视角链路逐环节排查方案.md`](lower-machine/档案/第二视角链路逐环节排查方案.md)。

#### 3.4.2 打开图传并设置开机自启

```bash
cd ~/my_lf_INF_20261025
sudo bash scripts/enable_video_service.sh
```

这个脚本会：把 `video_send_enable` 改成 `true`、创建 systemd 服务
**`autoaim-20261025`**、停掉旧的 `autoaim.service`。

> ⚠️ **脚本执行后视频帧就会开始往串口发。** 如果下位机还是没改过转发逻辑的旧固件，
> 309 字节视频帧会被当成控制帧解析，云台会受影响。**先烧固件，再开图传。**

#### 3.4.3 验收集

```bash
grep -a "VideoSender" logs/autostart.log | tail -n 8
```

`grep` 的 `-a` **不能省**：相机 SDK 会往日志里写 NUL 字节，不加 `-a` 会报
`binary file matches` 却什么都不打。

正常应该看到：

```
[VideoSender] 初始化完成 192x152 @20fps 59kbps preset=slow sharpen=0.00 灰度=是 profile=High 4:0:0(自动)
[VideoSender] 大帧拆包模式（I+P）：GOP=10 单帧上限 2368 字节（最多 8 包），令牌桶 40包/秒（码率按其中 85% 给 = 约 34.0 包/秒），VBV=18944 bit。
[VideoSender] 大帧统计：成帧 100（I 10 / P 90）超限丢弃 0（I 0） 出包丢包 0 编码错误 0  最近帧 312 字节 / 2 包
```

**只看三个指标**：

| 指标 | 正常 | 异常怎么办 |
|---|---|---|
| `成帧` 每 5 秒涨约 100，`I` 涨约 10 | 20fps / GOP10 生效 | 涨得慢 → `video_x264_preset` 改 `medium` |
| `出包丢包` 恒为 0 | 令牌桶撑得住（预算 88.8%） | **涨了就是码率超了**：`video_bitrate_kbps` 改成 `55` |
| `超限丢弃 0（I 0）` | 最坏一帧 6 包 < 8 | 涨了 → `video_max_frame_packets` 提到 `10` |

然后在客户端按 `H` 切到自定义流，画面应该出来（灰度是正常的）。

#### 3.4.4 如果客户端收不到画面

按这个顺序隔离，**每步只改一个变量**：

1. **Jetson 出帧了吗** → 上面那三行统计。`成帧` 不动 = 问题在 Jetson。
2. **STM32 收到了吗** → 用 `诊断工具/listen0310.bat` 旁听 PG14，看有没有 309 字节帧。
   有 = 板子在转发；没有 = 板子没收到或没转发。
3. **VT03 收到了吗** → VT03 的指示灯（型号不同灯语不同，见手册）。
4. **选手端转发了吗** → `诊断工具/dumpcap.bat` 抓 MQTT，找 `CustomByteBlock` topic。
5. **客户端解出来了吗** → 客户端日志里的 `cbb pkt_in` 计数。

完整的逐跳排查方案在
[`lower-machine/档案/第二视角链路逐环节排查方案.md`](lower-machine/档案/第二视角链路逐环节排查方案.md)。

### 3.5 复现清单（照这个打勾）

```
[ ] 客户端能登录、能看到官方图传                      ← 与车无关
[ ] 固件编译 0 Error + 烧录 Programming Done
[ ] 相机出图（video_sender_test，不依赖模型/CAN）
[ ] 模型文件就位（engine 必须本机生成）
[ ] 相机内参标定 → camera_matrix / distort_coeffs
[ ] 手眼标定 → R_camera2gimbal / t_camera2gimbal
[ ] R_gimbal2imubody 确认
[ ] 弹速 + yaw_offset/pitch_offset 打靶校准
[ ] 自瞄能跟住移动装甲板（auto_fire: false）
[ ] enable_video_service.sh + 三项统计正常
[ ] 客户端按 H 能看到本车相机画面
[ ] auto_fire: true，实弹命中
```

---

## 四、换车必须重做的参数 ★

**这是整份文档最重要的一节。** 下面每一项都是**这台车专属**，
照抄我们的值在你车上一定是错的。

| 参数 | 位置 | 我们的值 | 怎么改 |
|---|---|---|---|
| `camera_matrix` | `configs/standard3.yaml` | `[1785.49, 0, 672.48, 0, 1785.03, 559.90, 0, 0, 1]` | `calibrate_camera` |
| `distort_coeffs` | 同上 | `[-0.076, 0.112, 0.0005, -0.0028, 0]` | 同上 |
| `R_camera2gimbal` / `t_camera2gimbal` | 同上 | 9 + 3 个数 | `calibrate_handeye` |
| `R_gimbal2imubody` | 同上 | 单位阵 | 按云台装车方向手填 |
| `yaw_offset` | 同上 | `-2.57` | 打靶试射，见下 |
| `pitch_offset` | 同上 | `6.5` | 打靶试射 |
| 弹速 | `tasks/auto_aim/` 编译期常量 | 27.0 m/s（planner 钳 22） | 测速仪实测后改源码重编 |
| 相机型号与 `vid_pid` | `configs/standard3.yaml` | 大恒 `2ba2:4d55` | 换相机就改（海康 `2bdf:0001`、迈德威视 `f622:d13a`） |
| `exposure_ms` / `gain` | 同上 | 6 / 16.9 | 按现场光照重调 |
| CAN ID | 同上 | `quaternion 0x100 / bullet_speed 0x101 / send 0xff` | 跟电控对齐 |
| `com_port` | 同上 | `/dev/ttyACM0` | 插上 USB 后 `ls /dev/tty*` |
| 底盘运动学 | `chassis_project/application` | 全向轮 | 按实际轮系改 |
| 云台电机方向/限位 | `gimbal_project/application` | — | 上电前必须确认，否则会打限位 |
| VT03 接哪个串口 | `gimbal_project/Src/usart.c` | USART6 / PG14 / 921600 | 按实际接线改 |

**`yaw_offset` / `pitch_offset` 怎么定**（2026-10-07 我们在这上面吃过一次亏）：
固定靶、`auto_fire: true`、打 10 发，看弹着点相对准星的平均偏差，
按「1 米距离上 1 cm ≈ 0.57°」折算成角度改 `yaw_offset`。
如果偏差是**恒定方向**的，改这个；如果**时大时小**，是标定（第 2、3 项）的问题，别硬调。

---

## 五、全链路：一帧画面从相机到屏幕

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
| ② | 整幅等比缩到 192×152 → 灰度 → H.264 **I+P，GOP10**，单帧 ≤2368 B | `io/video_sender/video_sender.cpp` | ✅ |
| ③ | USB CDC，与 32 字节控制帧共用一条线 | `src/standard_mpc.cpp` + `io/gimbal` | ✅ 短写已修（10-24） |
| ④ | 按帧头 `0xFF`/`0xA5` 互斥分流 → CRC8/CRC16 → 8 帧队列 | `gimbal_project/application/Self_aim.c`、`video_forward.c` | ✅ |
| ⑤ | USART6 / **PG14** / 921600 8N1 发给 VT03 | `video_forward.c` | ✅ `HAL_UART_Transmit` 返回 OK |
| ⑥ | VT03 → 无线 → VT13 | 厂商硬件 | ⚠️ **通，但通过能力明显低于我们的发包率（疑似瓶颈）** |
| ⑦⑧⑨ | 选手端发布 MQTT `CustomByteBlock`（QoS0） | 选手端 | ✅ 抓包与客户端计数一致 |
| ⑩ | 客户端解包 → FFmpeg 解码 → 纹理显示 | `src/video/custom_byte_block_video_decoder.cpp` | ✅ |

### 协议天花板 vs 我们实际用的（都来自实测）

| 项 | 值 |
|---|---|
| **协议上限** | 0x0310 数据段 300 B，机器人→客户端**频率上限 50 Hz**，**无重传** → 50×300 = **15,000 B/s ≈ 120 kbps** |
| 外层帧长 | 309 B = 7B 头 + 300B data + 2B CRC16 |
| 内层封包 | 2B 长度 + 2B 序号 + 载荷 ≤ **296 B** |
| 我们发的 | 20 fps 编码，**59.6 kbps**，令牌桶 40 包/秒取 85% = 34 包/秒预算 |
| 实测落点 | 35.5 包/秒（令牌桶的 **88.8%**），单帧中位 312 B，最坏 6 包 |
| 单帧上限 | 8 包 × 296 B = 2368 B；VBV = 8×296×8 = **18944 bit** |

> **2026-10-28 之前的画质瓶颈是我们自己的代码，不是协议。**
> 旧代码把码率预算钉死在「每帧 1 个包 = 42.6 kbps」，只用了协议能力的 35%。
> 改成令牌桶 85% 口径后翻到 59.6 kbps，画面从「一坨糊」变成「能看清」。
> 详细推导见 [`autoaim/档案/包率85%+灰度版-部署与回退-2026-10-28.md`](autoaim/档案/包率85%+灰度版-部署与回退-2026-10-28.md)。

---

## 六、已知边界与坑

1. **帧率低，怀疑在图传那一跳（未证实）。** 我们发送端稳定 20 fps 出帧、令牌桶用 88.8%，
   但客户端实际出画明显更低。**目前只是怀疑 VT03→VT13 的无线通过能力**，
   没有测量证据——固件里已经放了 `volatile VideoForwardDebug_t VideoForward_Debug`
   （`video_forward.c`），烧上去在 Keil Watch 展开就能对比 `frames`（USB 收到的）
   和 `forwarded`（真写进 USART6 的），这是唯一能把「板子」和「图传」分开的测量。
   **不要把这条当结论引用。**
2. **别按 CTRL。** 固件把「上一轮和本轮都按着 CTRL」当复位信号，经 CAN `0x04` 第 4 字节
   让底板执行 `System_Reset()`——整车硬重启，云台不再跟随、粘着底盘，图传和选手端一起重来，
   **必须重开客户端**。切输入法用 Ctrl+Space 也会踩到。
   修法（改成按住 500ms 才触发）在电控那边编译报错，**已还原、不烧录**，现场就靠避开。
3. **灰度没有颜色。** 红蓝灯条只剩亮度差。如果队友需要按颜色判断，改 `video_gray: false`，
   代价是亮度 SSIM 掉约 0.001（几乎看不出来）。
4. **GOP10 意味着丢 I 帧后最多花屏 0.5 秒。** 这是 I+P 的固有代价，想更快恢复就 `video_gop: 5`。
5. **`H` 切主画面、`I` 翻转官方画面。** 官方 UDP 一断，客户端会临时把主画面让给自定义流。
6. **小电脑上的 yaml 改完不用重编**，`systemctl restart autoaim-20261025` 即可；改 C++ 才要重编。
7. **不要跑自瞄的全量 `make`**：`fire_test.cpp` 是遗留问题会编译失败，只编 `standard_mpc` 和 `video_sender_test`。
8. **`rm0310.py` 需要 Python**，CH 那台 Windows 没装，所以在别的机器上跑。
   其他诊断工具（`.bat`/`.ps1`）双击就能用。

---

## 七、这台车的实测数据（供对照，不要照抄）

| 项 | 值 |
|---|---|
| 相机 | 大恒，1280×1024，曝光 6 ms，增益 16.9 |
| 取景 | 整幅等比缩到 **192×152**（`keep_aspect: true`，无裁切） |
| 编码 | H.264 **High 4:0:0**（灰度），preset=slow，tune=zerolatency，slices=4，GOP10，无 B 帧 |
| 码率 | 59.6 kbps（自动，`video_bitrate_kbps: 0`） |
| 画质 | SSIM-Y **0.9853**（300 帧真实赛场画面对比同一编码器输入） |
| 发包 | 35.5 包/秒，令牌桶 40 的 88.8%，60 秒仿真 0 丢帧 |
| 解码端 | 客户端 FFmpeg libavcodec 62 软件解码，High 4:0:0 正常 |

---

## 八、下一步（按收益排序）

1. **测 ⑥ 这一跳。** 烧带 `VideoForward_Debug` 的固件，在 Keil Watch 读
   `frames` 与 `forwarded`：两个一起涨差距小 → 丢在 VT03 之后；
   `forwarded` 远小于 `frames` → 丢在板子转发（再看 `overflow` / `tx_error` / `tx_stuck`）。
2. **通道真有富余再提包率。** 现在 40 包/秒，协议上限 50。
   改 `video_send_rate_hz: 50` 能再拿 25% 带宽，但**必须实测通道吃不吃得下**，
   判据只看 `出包丢包` 是否还是 0。
3. **控制延迟。** 键鼠走 `CustomControl`（约 9~10 Hz），比官方通道慢，是下一个体验瓶颈。
4. **自瞄画质再往上**：`192×152` → `224×176` 已实测 SSIM 0.9886，但最好一帧要 13 包，
   超了 8 包上限，得同时把 `video_max_frame_packets` 提到 14 并重算令牌桶。

---

## 九、文档地图

| 想干什么 | 看哪 |
|---|---|
| 跑起来客户端 | [`luojiamaster/使用说明.md`](luojiamaster/使用说明.md) |
| 从零编译客户端 / 相对原版改了什么 | [`luojiamaster/完整复现指南.md`](luojiamaster/完整复现指南.md) |
| 本地模拟比赛、接线与 IP | [`luojiamaster/局域网联调手册.md`](luojiamaster/局域网联调手册.md) |
| 部署/编译自瞄、图传参数 | [`autoaim/README.md`](autoaim/README.md) |
| 固件协议、接口、编译烧录 | [`lower-machine/README.md`](lower-machine/README.md) |
| 串口/MQTT 诊断工具怎么用 | [`lower-machine/诊断工具/`](lower-machine/诊断工具/) |
| 画质为什么能到这一步、怎么回退 | [`autoaim/档案/包率85%+灰度版-部署与回退-2026-10-28.md`](autoaim/档案/包率85%+灰度版-部署与回退-2026-10-28.md) |
| 过去踩过的坑、被推翻的结论 | 各子系统的 `档案/README.md`（**动手前先看这个**） |

---

## 十、仓库卫生规则

- 编译产物、日志、IDE 配置**不进仓库**（`.gitignore` 已覆盖）。
- `boost` 因单文件 103.5 MB 超 GitHub 100 MB 上限不进仓库，首次编译由 CMake 自动下载
  （网络被掐时见 [`luojiamaster/完整复现指南.md`](luojiamaster/完整复现指南.md) 文末的离线方案）。
- `autoaim/**/assets/` 不进仓库（136 MB），换机器必须自己准备模型，见 3.3.2。
- 抓包 `.bin` 只保留 `lower-machine/诊断工具/` 里那 4 个小样本（合计数十 KB，供 `rm0310.py` 复算）；
  现场新抓的大文件不要入库。
- `程序/luojiamaster.exe` 是**唯一入库的二进制**，方便没编译环境的人直接用；重编后要同步覆盖。
- 新增一份「探索结论」文档时：先在对应子系统根目录放，**结题时移进 `档案/`**，
  并在 `档案/README.md` 时间线顶部加一行。
- 拿不准的东西写「可能性 / 待验证」，不要写成结论；结论被推翻时在档案里**追加**更正表。

---

## 许可与来源

自瞄部分基于[同济大学 SuperPower 战队 25 赛季自瞄算法](https://github.com/TongjiSuperPower)二次开发；
客户端基于 ACE 原版二次开发。均沿用原项目 LICENSE，仅用于技术交流。
