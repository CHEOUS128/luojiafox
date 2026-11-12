# 自瞄：Jetson 完整程序与 B 档视频

**当前版本为 2026-11-12 实车成功组合。** 本车运行完整自瞄程序，同时用 B 档发送彩色工业相机画面；云台板改为 DMA 转发后，用户已确认官方图传到自定义客户端正常出画。

日常开机和维护统一看 [实车成功版本](../docs/2026-11-12-实车成功版本.md)。本轮不改变自瞄算法、模型和标定，不需要重复部署。

## 源码和本车实际目录

| 项目 | 当前值 |
| --- | --- |
| 仓库源码 | `my_lf_INF_20261025/`，包括 `cw-trial-20261109-r2` 视频入口 |
| 本车现役目录 | `/home/nvidia4/my_lf_INF_20261025_cwtrial` |
| 自启动 | `autoaim-20261025.service`，覆盖配置 `40-cw-trial.conf` 选择现役目录 |
| 启动参数 | `build_cw/standard_mpc configs/standard3.yaml --cw-profile=quality` |
| 当前视频 | B 档：彩色、全视野、320×256、目标20 fps、H.264、GOP20 |
| 固定预算 | 300 B数据段/309 B外层，最多2包/592 B H.264，最高50包/秒，逐包至少20 ms |
| 保留的旧版 | 车上 `my_lf_INF_20261025` 原目录及 `my_lf_INF_20261013`；仓库旧版本不修改 |

`quality` 会覆盖视频尺寸、颜色、GOP和包数等参数；YAML里仍保留的灰度、8包参数属于未选择试验档时的旧流程，不能把它当作本车现在的画面设置。仿真对应配置为 `compression-war/profiles/one-320-gop20.ini`，原始对照仍为 `baseline.ini`。

现役服务保留本车 `configs/standard3.yaml`、模型和标定。`my_sp_main/io/daheng/lib/armv8` 提供仍被依赖的 `liblog4cplus_gx.so`，这个旧目录不能清掉。

## 视频链路

```text
相机 → 完整自瞄程序取图、B档编码 → 0x0310 / USB CDC
→ 云台C板校验与TX DMA → VT03 → VT13 → 官方选手端 → 自定义客户端
```

32 B自瞄控制和309 B视频继续共用USB，串口互斥、短写续传和控制帧边界保留。C板本轮修改见 [电控 README](../lower-machine/README.md)。早期单包、灰度和多包方案的参数、经验留在 [档案](档案/README.md)，旧“怀疑无线链路”的判断不再作为本次故障结论。

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

## 既有算法维护记录

以下算法事项保留原审核记录，本轮没有修改算法或重新评估这些事项。旧 V1/V3 视频项已由 B 彩色档和 C 板 DMA 修复接替，见当前版本说明。

| 编号 | 问题 | 影响 |
|---|---|---|
| P4 | `MATCH_SCORE_LIMIT` 注释说 0.15，实际 0.30 | 可能放进坏观测 |
| W1 | 关键参数是编译期常量，不能从 yaml 调 | 换车需重编 |
| W2 | `use_phase_jump_guard` 默认 false 且配置中缺失 | 换板保护从未生效 |
| W3 | 三阶指令的 vel/acc 未过滤波限速 | 下位机前馈可能过冲 |
| W4 | 双模型是「注入速度」的混合做法，非完整版 | r 可能漂移 |
| V2 | 弹速写死 27.0 m/s，planner 钳 22 | 换发射机构必须改源码重编 |

---

## 编译和运行维护

现役机器已经配置开机自启动。先用以下只读命令确认状态，不要再启动第二个进程抢相机、串口：

```bash
systemctl show autoaim-20261025 -p ActiveState -p ExecStart -p WorkingDirectory -p DropInPaths
tail -n 30 /home/nvidia4/my_lf_INF_20261025_cwtrial/logs/autostart.log
```

维护或换电脑时先核对配置、模型、相机SDK、CUDA/OpenCV和共享库，具体成功配置及“传什么→传到哪→怎么验证”集中在 [统一维护说明](../docs/2026-11-12-实车成功版本.md)。只编 `standard_mpc` 和 `video_sender_test`，不跑全量 `make`。

`video_sender_test` 是本地编码检查，不向C板串口发送；合成模式不需要接车。在已编好的现役目录可执行：

```bash
cd /home/nvidia4/my_lf_INF_20261025_cwtrial
./build_cw/video_sender_test /tmp/cw-quality-check --cw-profile quality --seconds 15
python3 tests/verify_frames.py /tmp/cw-quality-check/video_test_frames.bin tools/crc.cpp
```

真实相机检查需要相机可用，维护时避免与现役程序同时占用。源码文件继续通过 ToDesk 手动传输；原有 `scripts/enable_video_service.sh` 属于旧服务部署入口，当前维护不重跑它。切换现役或回退需核对实际服务覆盖配置，避免只改base service却没有切换生效。

## 许可与来源

本项目基于 [同济大学 SuperPower 战队 25 赛季自瞄算法](https://github.com/TongjiSuperPower) 二次开发，沿用原项目 LICENSE。当前调校针对26赛季全向轮步兵，换兵种必须重新适配和标定。
