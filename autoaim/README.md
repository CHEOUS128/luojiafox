# 自瞄算法（autoaim）

> 珞珈狐战队 · RM26 赛季自瞄算法
>
> **本自瞄目前针对 26 赛季全向轮步兵调校。**
> 其他兵种（英雄、哨兵、空中、工程）的配置与参数未做针对性适配，直接套用需重新标定。

---

## 目录

```
autoaim/
├── my_lf_INF_20261025/          ← 当前版本（含图传发送端，实车在跑）
├── my_lf_INF_20261013/          ← 上一版（一字节未动，回滚用）
└── 档案/                        ← 探索过程：图传 v2/v3、单包模式、主循环卡顿、电控改造说明
```

**当前状态（2026-10-28）**：工业相机画面已经能在自定义客户端显示。
发送端稳定 20 fps 出帧、令牌桶用 88.8%、SSIM-Y 0.9853；
客户端实际出画帧率明显低于 20，**怀疑在 VT03→VT13 那一跳（未证实）**。
全过程结论见 [`档案/README.md`](档案/README.md)。

## 版本说明

| 版本 | 日期 | 说明 |
|---|---|---|
| `my_lf_INF_20261025` | 2026-10-20 | **当前版本**：新增双摄像头图传发送端（工业相机画面 → 自定义客户端）；10-28 改 I+P 大帧 + 令牌桶 85% + 灰度 |
| `my_lf_INF_20261013` | 2026-10-13 | 基于 10.04 版本逐行审核后的修复与增强版（回滚用，未改动） |

> **版本规则**：每个版本一个独立目录，老版本不做任何修改，用于回滚。

完整更新内容见 `my_lf_INF_20261025/更新日志.md`。

---

## 本版新增：双摄像头图传

**做什么**：把自瞄工业相机的画面送到自定义客户端的第二视角（客户端按 `H` 键切换）。

```
大恒相机 → 自瞄程序 → 整幅等比缩放 192×152 → 转灰度 → H.264（I+P，GOP10，独立线程）
    → 按客户端格式封装 → 0x0310 帧（309 字节，一帧拆 1~6 包）→ USB CDC（与云台控制共用）
    → STM32 主控（电控按帧头分流 + 原样转发）
    → VT03 图传发送端 → 无线 → VT13 → 选手端 → 自定义客户端
```

**当前状态**：视觉端已完成并实车验证；电控侧接收转发已烧录；客户端已能显示。

**关键参数**（`my_lf_INF_20261025/configs/standard3.yaml`，**实车在用的值**）：

| 项 | 值 |
|---|---|
| 取景 | 整幅等比缩放到 192×152（`keep_aspect: true`，**不裁切**，全视野） |
| 帧率 | 20 fps 编码 |
| 码率 | **59.6 kbps**（自动，`video_bitrate_kbps: 0`；= 令牌桶 40×85% ÷ 实测系数 1.35） |
| 编码 | H.264 **High 4:0:0**（灰度），preset=slow，tune=zerolatency，slices=4，无 B 帧 |
| GOP | **10**（I 帧 + 9 个 P 帧；丢 I 帧最长花屏 0.5 s） |
| 单包载荷上限 | **296 B**（= 300 − 2B 内层长度 − 2B 内层序号） |
| 单帧上限 | **8 包 × 296 B = 2368 B**；VBV = 8×296×8 = 18944 bit |
| 发送节拍 | 40 包/秒令牌桶，取 85% 作预算；实测落 35.5 包/秒（88.8%） |

**画质**：SSIM-Y **0.9853**（300 帧真实赛场画面，对比同一编码器输入）。
同一个 `档案/` 里还记了：160×128 灰度 70kbps 是 0.9898、
224×176 灰度 70kbps 是 0.9886 但最好一帧要 13 包（超 8 包上限）。

**想调的话**（每改一项只看 `出包丢包` 是否还是 0）：

| 想要 | 改什么 | 代价 |
|---|---|---|
| 更清楚 | `video_width/height` 提到 224×176 | 需同时把 `video_max_frame_packets` 提到 14 |
| 更多比特 | `video_send_rate_hz` 40 → 50 | 协议上限 50；通道吃不吃得下未测 |
| 恢复更快 | `video_gop` 10 → 5 | 每帧字节多约 10% |
| 有颜色 | `video_gray: false` | 亮度 SSIM 掉约 0.001 |

**相关文档**：

| 文件 | 给谁看 | 内容 |
|---|---|---|
| [`档案/README.md`](档案/README.md) | 全员 | ★ 图传 v2/v3、单包模式、I+P、包率 85%、主循环卡顿——**含被推翻的结论** |
| [`档案/包率85%+灰度版-部署与回退-2026-10-28.md`](档案/包率85%+灰度版-部署与回退-2026-10-28.md) | 视觉/部署 | **当前版本**：为什么 60kbps、灰度值多少、三层回退 |
| [`档案/I+P大帧版-部署与回退-2026-10-28.md`](档案/I+P大帧版-部署与回退-2026-10-28.md) | 视觉/部署 | I+P 拆包怎么来的（上午那版，已被上面取代） |
| [`档案/双摄像头-电控改造说明.md`](档案/双摄像头-电控改造说明.md) | **电控** | 0x0310 帧格式、接入点、CRC 表 |
| [`档案/主循环卡顿诊断-2026-10-23.md`](档案/主循环卡顿诊断-2026-10-23.md) | 视觉/部署 | `成帧` 不涨、`Current FPS: 0.0` 时看这份 |
| [`my_lf_INF_20261025/更新日志.md`](my_lf_INF_20261025/更新日志.md) | 全员 | 本版全部改动与技术细节 |

---

## 换车必须重做的标定（★ 最重要）

**这套代码的算法可以直接搬，参数不能。** 下面每项都是这台车专属，照抄一定打不中。
全部改在 `my_lf_INF_20261025/configs/standard3.yaml`，它们决定的是物理量，不是风格偏好。

| # | 标定什么 | 用什么程序 | 写回哪个键 | 不做的后果 |
|---|---|---|---|---|
| 1 | 相机内参 | `calibrate_camera` | `camera_matrix` / `distort_coeffs` | 距离和角度全错 |
| 2 | 手眼（相机→云台） | `calibrate_handeye` | `R_camera2gimbal` / `t_camera2gimbal` | 近准远偏，转身就飘 |
| 3 | 云台→IMU 旋转 | 手填 | `R_gimbal2imubody` | pitch 补偿反向 |
| 4 | 弹道偏移 | 打靶实测 | `yaw_offset` / `pitch_offset` | 准星对得上但弹着点恒偏 |
| 5 | 弹速 | 编译期常量（源码） | — | 提前量全错 |

### 1~2 的采集与计算

标定板是**棋盘格**，格数与方格边长写在 `configs/calibration.yaml`
（默认 `pattern_cols: 8 / pattern_rows: 6 / square_size_mm: 29`，
`calibrate_camera` 内部按「格子数 − 1」算内角点，即 7×5）。

> ⚠️ **换板子要改两个地方，只改 yaml 不够。**
> `calibration/capture_gimbal.cpp:126` 把内角点数**硬编码**成
> `const cv::Size patternSize(5, 7);`，**不读 calibration.yaml**。
> 而且它和 yaml 推出的是**转置关系**（5×7 vs 7×5）——棋盘格检测能容忍转置，
> 但你换成别的格数时，采集端不会跟着变，只会一直识别失败。
> 换板子就把这一行改成你的内角点数，然后重编 `capture_gimbal`。
>
> **必须和实物一致**，不一致会得到一堆识别失败（`calibrate_camera` 会直接报
> 「有效数据不足，至少需要8组」）。

```bash
cd my_lf_INF_20261025

# ① 采集：按 s 存图（同时存四元数），按 q 退出。摆 15~20 个不同姿态/角度
./build/capture_gimbal -c configs/calibration.yaml
#    → assets/img_with_q/1.jpg, 1.txt, 2.jpg, 2.txt ...

# ② 相机内参（至少 8 组，程序会检查）
./build/calibrate_camera assets/img_with_q -c configs/calibration.yaml
#    → 屏幕打印 yaml 格式的 camera_matrix / distort_coeffs，手工贴回 standard3.yaml
#    → 看「重投影误差」，< 0.5 px 算好；> 1 px 说明图拍得不好，重拍

# ③ 手眼（相机装在云台上，要云台转起来拍同一个标定板）
./build/calibrate_handeye assets/img_with_q -c configs/calibration.yaml
#    → 屏幕打印 R_camera2gimbal / t_camera2gimbal，贴回 standard3.yaml

# （可选）机器人坐标系手眼，用于验证整链
./build/calibrate_robotworld_handeye assets/img_with_q -c configs/calibration.yaml
```

> `capture` 那个程序用的是**圆点阵列**（10×7 对称圆点），和棋盘格标定流程不是一套，
> 别混用。而且它同时构造 `io::CBoard`，没接 C 板会直接失败。

### 3. 云台→IMU 旋转

`R_gimbal2imubody` 是 3×3 旋转矩阵，9 个数。它描述「云台坐标系的轴在 IMU 坐标系里指向哪」。
用 `capture_gimbal` 显示的三轴欧拉角辅助判断：云台 pitch 抬高时，画面里的 Z/Y/X 读数应该怎么变。
填错的典型现象是**上下瞄准反向**。我们车上用的是单位阵
（`[1,0,0, 0,1,0, 0,0,1]`），换车几乎一定不是。

### 4. yaw_offset / pitch_offset（弹着点校准）

**固定靶、`auto_fire: true`、打 10 发**，看弹着点相对准星的平均偏差：

- 偏差**方向恒定** → 改这两个 offset。1 米距离上 1 cm ≈ 0.57°。
- 偏差**时大时小** → 不是 offset 的问题，是第 1、2 项标定没做好，别硬调。

我们的值：`yaw_offset: -2.57`、`pitch_offset: 6.5`。
（2026-10-07 在这上面吃过一次亏：一个 0.57° 的恒定水平偏差让静止靶一直打不中。）

### 5. 弹速

写死在源码里的编译期常量，当前 **27.0 m/s**，planner 钳到 22。
换发射机构必须用测速仪实测后改源码重编。这是已知欠债（见「已知未处理项」W1）。

---

## 目标平台：26 赛季全向轮步兵

| 项目 | 配置 |
|---|---|
| 兵种 | 步兵（全向轮底盘） |
| 相机 | 大恒（Daheng）/ 海康（HikRobot）/ MindVision，代码支持多品牌 |
| 相机分辨率 | 1280×1024 |
| 运行平台 | Jetson Orin（ROS2 可选） |
| 下位机 | STM32（见 `lower-machine/`） |
| 通信 | 串口（控制帧 32 字节 `0xFF`；视频帧 309 字节 `0xA5`） |

**注意**：以下参数与具体车辆强相关，换车必须重新标定：

- `yaw_offset`（弹着点校准，2.5m 实测值；2026-10-07 修过一次 0.57° 水平偏差）
- `pitch_offset`（垂直校准）
- 弹速（当前写死 27.0，planner 钳到 22）
- 相机内参与手眼标定

---

## 核心改动（相对 10.04 版本，20261013 起）

### 1. 双模型跟踪（装甲板 KF）

在 `Target` 中并行新增一个**装甲板线性卡尔曼滤波**（6 维：x/y/z 及速度）。
平动时（|ω| < 0.7 rad/s）用装甲板 KF 的速度替代整车 EKF 速度，避免旋转模型污染。

### 2. 修复 3 个实际缺陷

| 编号 | 问题 | 修复 |
|---|---|---|
| P1 | 装甲板 KF 的 Q 矩阵结构错误 | 改为按 dt 规范缩放 `[dt⁴/4, dt³/2; dt³/2, dt²] × Q` |
| P2 | 两套「平动修正」互相打架 | 去掉 freeze，只保留一套机制 |
| P3 | 图像中心硬编码为 `(720, 540)` | 改为 `(640, 512)` |

### 3. 可观测性

新增 `[DualModel]` 日志，输出 `armor_vy / vehicle_vy / w / r`。

### 4. 三阶指令管道（协议已就绪，可开启）

`gimbal.cpp` 已具备发送「角度 + 角速度 + 角加速度」（32 字节帧）的能力，
受 `three_order_enable` 开关控制。下位机 10.14 版本已支持三阶帧解析。

---

## 已知未处理项

| 编号 | 问题 | 影响 |
|---|---|---|
| P4 | `MATCH_SCORE_LIMIT` 注释说 0.15，实际 0.30 | 可能放进坏观测 |
| W1 | 关键参数是编译期常量，不能从 yaml 调 | 换车需重编 |
| W2 | `use_phase_jump_guard` 默认 false 且配置中缺失 | 换板保护从未生效 |
| W3 | 三阶指令的 vel/acc 未过滤波限速 | 下位机前馈可能过冲 |
| W4 | 双模型是「注入速度」的混合做法，非完整版 | r 可能漂移 |
| V1 | **客户端实际出画帧率明显低于 20 fps** | **怀疑在 VT03→VT13 那一跳，未证实**；固件里 `VideoForward_Debug` 是唯一的区分测量 |
| V2 | 弹速写死 27.0 m/s，planner 钳 22 | 换发射机构必须改源码重编 |
| V3 | 灰度模式无颜色 | 红蓝灯条只剩亮度差，`video_gray: false` 可退 |

---

## 编译

### 环境

- Ubuntu 22.04
- OpenVINO / TensorRT（按实际部署）
- Ceres（优化）
- FFmpeg 开发库（图传用：`libavcodec-dev libswscale-dev libavutil-dev libx264-dev`）
- 相机 SDK（大恒 / 海康 / MindVision，按实际相机选装）

### 步骤

```bash
cd my_lf_INF_20261025
mkdir -p build && cd build
cmake ..
make standard_mpc video_sender_test -j$(nproc)
```

> ⚠️ 不要跑全量 `make`：`fire_test.cpp` 是项目遗留问题，会编译失败。
> 生产目标为 `standard_mpc`；验证程序为 `video_sender_test`。

### 运行

```bash
./standard_mpc ../configs/standard3.yaml
```

### 图传验证（不接主控）

`video_sender_test` 只依赖相机和编码器——**不碰 YOLO、不碰 CAN、不碰云台串口**，
是这套代码里最干净的「相机是否出图」体检。没装模型、没接 C 板也能跑。

```bash
# 合成图模式（连相机都不用，先确认编译和编码链路没问题）
./build/video_sender_test /tmp

# 真实相机 + 取景对比图（必须 sudo，logs/ 是 root 的）
sudo ./build/video_sender_test /tmp -c configs/standard3.yaml --compare
#   → /tmp/video_test_view.png   取景+锐化后的画面
#   → /tmp/video_test_stream.h264 纯码流，可以 ffplay 看
#   → /tmp/video_test_frames.bin  完整 0x0310 帧序列

# 帧格式校验（CRC / 长度 / 序号）
python3 tests/verify_frames.py /tmp/video_test_frames.bin tools/crc.cpp
```

可以临时覆盖参数做 A/B，不用改 yaml：

```bash
./build/video_sender_test /tmp -c configs/standard3.yaml \
  --fps 20 --kbps 80 --gray 0 --gop 5 --large-frame 0
```

---

## 部署（Jetson）

| 项目 | 配置 |
|---|---|
| 代码目录 | `/home/nvidia4/my_lf_INF_20261025` |
| 自启动 | systemd 服务 **`autoaim-20261025`**（`scripts/enable_video_service.sh` 创建，开机 15 秒后启动） |
| 配置文件 | `configs/standard3.yaml`（**改完不用重编**，`systemctl restart` 即可） |
| 日志 | `logs/autostart.log`（含二进制字节，grep 要加 `-a`） |
| 传文件 | ToDesk（Jetson 直连 GitHub 会超时，必须选覆盖并 grep 校验） |

常用命令：

```bash
systemctl status autoaim-20261025 --no-pager | head -8
sudo systemctl restart autoaim-20261025
tail -f logs/autostart.log | grep -a --line-buffered "VideoSender"
sudo bash scripts/rollback_to_20261013.sh     # 回滚到无图传的上一版
```

> 早期文档里写的服务名 `autoaim` 指的是 20261013 那一版；现在两版并存，
> 20261025 用 `autoaim-20261025`。回滚脚本会切回去。

详细步骤见 [`档案/图传v2-部署与验证手册.md`](档案/图传v2-部署与验证手册.md)。

---

## 许可与来源

本项目基于 [同济大学 SuperPower 战队 25 赛季自瞄算法](https://github.com/TongjiSuperPower) 二次开发，
沿用原项目 LICENSE。仅用于技术交流。
