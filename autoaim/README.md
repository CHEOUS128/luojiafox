# 自瞄与 Jetson 现役部署

本目录负责相机、自瞄算法、模型、标定、云台通信及 B 档视频。**2026-11-12 本车已部署完整自瞄，云台 DMA 修复后，CH 确认工业相机画面经官方链路到自定义客户端正常。** 原算法、模型和本车标定继续沿用。

本页对应 2026 全向轮步兵参考车。2027 指挥席的三路录像和场馆位置样例都由 Windows 本机提供。在电脑上体验指挥席可直接看[客户端使用说明](../luojiamaster/README.md)。正常开机直接用已配置的服务，不要再启动第二个进程抢相机和USB。

**其他车辆只想接入自定义客户端或工业相机第二视角，先看 [跨车型部署教程](../luojiamaster/部署教程/README.md)。** 那里区分完整自瞄迁移与仅移植视频模块，并列出8文件更新包的范围、共用串口短写续传、姿态等待和档位选择等必查经验；本页维护这台参考车的真实目录与环境。

## 当前目录和视频参数

| 项目 | 本车实际值 |
| --- | --- |
| 仓库源码 | `my_lf_INF_20261025/`，视频版本标记 `cw-trial-20261109-r2` |
| 主机/用户 | `nvidia4` |
| 现役目录 | `/home/nvidia4/my_lf_INF_20261025_cwtrial` |
| 程序与参数 | `build_cw/standard_mpc configs/standard3.yaml --cw-profile=quality` |
| 自启动服务 | `autoaim-20261025.service`，root运行；覆盖文件 `/etc/systemd/system/autoaim-20261025.service.d/40-cw-trial.conf` |
| 启动等待 | 基础服务先等15秒，再最多等40秒，等待 `/dev/ttyACM0` 和相机 `2ba2:4d55` |
| 日志 | `/home/nvidia4/my_lf_INF_20261025_cwtrial/logs/autostart.log` |
| 视频 | B档：彩色、全视野、等比缩小补边，320×256、目标20 fps、H.264/GOP20，约74.5 kbps |
| 通信预算 | 300 B数据段含4 B内层头、最多296 B码流；309 B外层；含关键帧最多2包/592 B；最高50包/秒，逐包至少20 ms |
| 保留的原目录 | `/home/nvidia4/my_lf_INF_20261025` 与更早的 `my_lf_INF_20261013` |
| 不能清掉的依赖 | `/home/nvidia4/my_sp_main/io/daheng/lib/armv8/liblog4cplus_gx.so`，现役程序仍依赖它 |

`--cw-profile=quality` 覆盖视频参数；YAML仍显示灰度、8包等旧值时，不把它们当作现役视频档位。仿真同档为 `compression-war/profiles/one-320-gop20.ini`，原始对照为 `baseline.ini`。

```text
相机 → 完整自瞄和B档编码 → USB CDC → 云台校验/队列/TX DMA
→ VT03 → VT13 → 官方端 → CustomByteBlock → 自定义客户端
```

32 B自瞄控制与309 B视频共用USB，串口互斥、短写续传及控制帧边界保留。视频成功出画证明该视频链路可用，自瞄精度还要靠标定与打靶单独验证。云台源码、2391包实测及采集溢出见 [电控README](../lower-machine/README.md)；Windows操作见 [客户端README](../luojiamaster/README.md)。

## 日常开机与只读检查

相机、Jetson、云台按既有接线上电，服务自动运行。没有画面时，在Jetson终端先查看：

```bash
systemctl show autoaim-20261025 -p ActiveState -p SubState -p ExecStart -p WorkingDirectory -p DropInPaths
ls -l /dev/ttyACM0
lsusb -d 2ba2:4d55
tail -n 30 /home/nvidia4/my_lf_INF_20261025_cwtrial/logs/autostart.log
```

应看到 `active/running`、现役目录、`quality` 参数，USB和相机存在。记录异常时间和原始输出，按“Jetson成帧/写入 → 云台完整输出 → 官方端收包 → 客户端解码/显示”找第一处差异。含NUL的SDK日志需要筛选时使用 `grep -a`，避免只得到 `binary file matches`。

Jetson网口为 `enP8p1s0`，原地址 `192.168.1.50/24`。直连Windows维护时曾临时加 `10.88.26.2/24`，重启会消失；Windows对应 `10.88.26.1`。这是维护网络，不是客户端比赛IP。恢复官方客户端连接时改回12.x专线，具体看 [现场接线](../luojiamaster/工具/README.md)。

## 以后维护或换电脑

当前车已经部署好。以下操作用于安排好的停机维护，执行前保留本车 `configs/`、模型、标定、原目录和依赖，确认不会同时运行两个完整自瞄程序。

| 传什么 | 传到哪里 | 如何确认 |
| --- | --- | --- |
| 本次源码ZIP及同名 `.sha256` | 手动ToDesk传到 `/home/nvidia4/Downloads/`；核验后将包内8份源码对应更新到现役目录，保留 `configs/`、`assets/` | 对应新包SHA256；两个编译产物 `--help` 含 `cw-trial-20261109-r2`；记录时间、大小及SHA |
| 模型、配置或标定 | 仅按对应维护任务的明确清单传，不随源码包整目录覆盖 | 核对本车型号、文件SHA及配置引用路径 |
| 云台代码 | 从仓库取配套完整云台工程，由电控处理 | Keil工程、3份DMA源码及构建标记见电控README；烧录须CH确认 |

在Windows仓库根生成带时间的新包：

```powershell
$package = '.\compression-war\runs\delivery\RM26_jetson_B_' + (Get-Date -Format 'yyyyMMdd_HHmmss') + '.zip'
python '.\compression-war\tools\package_vehicle_trial.py' --out $package
```

打包器显示路径、SHA并生成同名 `.sha256`。手动ToDesk传这两个文件；Jetson在Downloads核验时换成这次真实名称：

```bash
tr -d '\r' < RM26_jetson_B_日期时间.sha256 | sha256sum -c -
```

在已准备好源码的Jetson现役目录，仅编两个生产目标：

```bash
cd /home/nvidia4/my_lf_INF_20261025_cwtrial
cmake -S . -B build_cw -G 'Unix Makefiles' \
  -DOpenCV_DIR=/usr/local/lib/cmake/opencv4 \
  -DCUDA_TOOLKIT_ROOT_DIR=/usr/local/cuda-12.6 \
  -DCMAKE_CUDA_COMPILER=/usr/local/cuda-12.6/bin/nvcc
make -C build_cw standard_mpc video_sender_test -j2
./build_cw/standard_mpc --help | grep 'cw-trial-20261109-r2'
./build_cw/video_sender_test --help | grep 'cw-trial-20261109-r2'
stat -c '%y %s %n' build_cw/standard_mpc build_cw/video_sender_test
sha256sum build_cw/standard_mpc build_cw/video_sender_test
```

不跑全量 `make`，遗留 `fire_test` 目标有构建问题。OpenCV、CUDA/TensorRT或OpenVINO及相机SDK按目标机器实际环境核对，不能拿Windows编码速度当Jetson速度。

模型不在仓库中；原生产识别会加载配置引用的模型，不能靠打开传统识别开关跳过模型准备。TensorRT engine与设备/版本有关，换机器须检查或重新生成，保留原ONNX及标定。配置涉及 `yolov5_model_path` 等实际键，按本车文件核对。

`video_sender_test`本地编码，不向C板串口发送；合成模式不需接车：

```bash
cd /home/nvidia4/my_lf_INF_20261025_cwtrial
./build_cw/video_sender_test /tmp/cw-quality-check --cw-profile quality --seconds 15
python3 tests/verify_frames.py /tmp/cw-quality-check/video_test_frames.bin tools/crc.cpp
```

真实相机检查需要相机空闲，不能与生产服务抢相机。编译完成不代表现役服务已切换；先读 `systemctl cat autoaim-20261025`。旧 `scripts/enable_video_service.sh` 是历史安装入口，不在当前机器反复重跑。需要回退时由维护负责人核对服务覆盖配置，切回保留的原目录并重新核验，不能只改base service就认定生效。

<details>
<summary>现役服务覆盖配置与11月12日版本校验记录</summary>

基础服务保留15秒等待及自动重试，`40-cw-trial.conf`增加设备等待并选择现役目录：

```ini
[Service]
WorkingDirectory=/home/nvidia4/my_lf_INF_20261025_cwtrial
ExecStartPre=/usr/bin/timeout 40s /bin/sh -c 'until test -c /dev/ttyACM0 && /usr/bin/lsusb -d 2ba2:4d55 >/dev/null 2>&1; do /bin/sleep 1; done'
ExecStart=
ExecStart=/home/nvidia4/my_lf_INF_20261025_cwtrial/build_cw/standard_mpc configs/standard3.yaml --cw-profile=quality
StandardOutput=append:/home/nvidia4/my_lf_INF_20261025_cwtrial/logs/autostart.log
StandardError=append:/home/nvidia4/my_lf_INF_20261025_cwtrial/logs/autostart.log
```

当时 `RM26_cw_trial_20261109_r2.zip` SHA256：`e6dcb827598847accc45389ac1865de5a9b9cd8f325c08723f4c5c7b1c33b390`；本车已编 `standard_mpc` SHA256：`9e4fef10f9c7ce68aac3ec70eb3f0bb915fbebb4d0394fdec68130beaccf2f50`。这些只对应11-12记录；重新打包或编译后使用新校验值，不能拿历史值要求新包相同。

</details>

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

**仅在已安排的打靶场地，由电控确认执行器及开火方式后对固定靶采样**，看弹着点相对准星的平均偏差。`standard_mpc`直接发送规划结果，不能把YAML的 `auto_fire` 当作整车禁火总开关。

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

## 许可与来源

本项目基于 [同济大学 SuperPower 战队25赛季自瞄](https://github.com/TongjiSuperPower) 二次开发，沿用原许可。当前参数针对26赛季全向轮步兵，换车需重新适配和标定。早期视频方案与更正见 [档案索引](档案/README.md)。
