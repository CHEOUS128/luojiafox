# 下位机代码（lower-machine）

> 珞珈狐战队 · RM26 赛季全向轮步兵固件
>
> **26 赛季全向轮步兵**的主控固件（STM32）。
>
为方便队员接手，本次只更新云台视频链路：
> **2026-11-09：修复视频错位后的重新同步，以及转发队列满载时的并发覆盖。**
> 控制协议与 `autoaim/my_lf_INF_20261025` 匹配，仍为32 B三阶指令和309 B的0x0310视频帧。
>
> 📄 本次修复已通过主机源码回归，**尚未进行ARMCC/Keil固件编译和实车验证**；车上仍是改动前版本。
> 📄 全过程结论：[`档案/README.md`](档案/README.md)（**含已被推翻的说法，先看再动手**）

---

## 目录

```
lower-machine/
├── 26赛季全向轮步兵/
│   ├── chassis_project/     ← 底板：底盘/功率/电容/复位执行
│   └── gimbal_project/      ← 云台板：云台/发射/IMU/USB CDC/视频转发
├── 诊断工具/                ← PC 端串口/MQTT 工具（Windows 自带 PowerShell 即可）
└── 档案/                    ← 探索过程：逐跳排查、代码核对、云台疯转修复、被推翻的结论
```

## 2026-11-09 本次改哪里

仅修改以下两个云台源码文件：

- `26赛季全向轮步兵/gimbal_project/application/Self_aim.c`：视频CRC失败时保留下一个合法帧头候选，避免持续错位。
- `26赛季全向轮步兵/gimbal_project/application/video_forward.c`：短临界区内复制并出队，再从独立静态缓冲发送，防止USB回调覆盖正在发送的数据。

电控拉取整个仓库即可，仍打开 **`26赛季全向轮步兵/gimbal_project/MDK-ARM/standard_robot.uvprojx`**，Target为 `standard_robot`，按原流程编译云台工程。工程文件、头文件、底盘工程和控制参数均未改，不需要新烧录脚本。

原工程配置为 ARM Compiler 5.06 update 7 build 960、STM32F407IGHx、Keil.STM32F4xx_DFP 3.1.1。本次主机回归通过15个用例和6组视频回放；实际固件编译、RAM/任务栈与控制实时性仍需电控确认。

改动与证据见 [云台视频链路修复记录](档案/2026-11-09-云台视频链路修复.md)。这两处代码缺陷已复现，**不等于实车故障全部根因已确认**。

## 两个工程的分工

| 工程 | 职责 |
|---|---|
| `chassis_project` | 底盘运动控制、功率管理、超级电容、底盘状态上报 |
| `gimbal_project` | 云台控制、发射机构、视觉通信、IMU 解算 |

---

## 图传接收侧（2026-10-20 新增）

`gimbal_project` 新增 0x0310 视频帧接收与转发：

```
USB CDC（Jetson）→ 按帧头分流（0xFF 控制帧 / 0xA5 视频帧）
    → 校验（长度 / cmd_id / CRC8 / CRC16）
    → 队列 → 原样转发到 VT03 图传发送端
```

改动文件：新增 `application/video_forward.c/.h`；修改 `Src/usbd_cdc_if.c`、
`application/Self_aim.c/.h`、`MDK-ARM/standard_robot.uvprojx`。

**需要确认**：VT03 接在 **USART6 / PG14**（`Src/usart.c` 里 921600 8N1）。
推导过程见 [`档案/图传接收侧改造说明.md`](档案/图传接收侧改造说明.md)。
当前 `main → remote_control_init → RC_Init` 实际启用了USART6接收DMA和IDLE中断，
不能再将 `USART6_IRQHandler` 当作未启用的死代码。`detect_task` 中恢复回调被注释，
只说明那条自动重启路径未注册，不代表接收初始化未执行。

**历史记录（2026-10-25）**：客户端曾能看到画面，平均约2帧/秒。
这不能证明当前链路每段都正常。2026-11-09离线已复现下面记录的两处C板缺陷；
实车还需同时核对 `VideoForward_Debug`、USART6线上完整帧与客户端收包。
`forwarded` 仅是HAL成功计数，不能替代线上CRC与载荷检查。

排查方法与历史记录全部在 [`档案/`](档案/)：

| 文档 | 用途 |
|---|---|
| [`档案/README.md`](档案/README.md) | **先看这份**：时间线 + 已被推翻的结论表 |
| [`档案/第二视角链路逐环节排查方案.md`](档案/第二视角链路逐环节排查方案.md) | 逐跳隔离排查（灯 → 波形 → PC 扮演接收端 → PC 扮演 STM32 → 真链路计数） |
| [`档案/电控代码核对-2026-10-24.md`](档案/电控代码核对-2026-10-24.md) | 历史核对与三轮抓包；部分推断已由2026-11-09记录更正 |
| [`档案/电控Debug入门手册.md`](档案/电控Debug入门手册.md) | **新手电控必读**：证据 vs 推测、三种调试手段各能证明什么 |

诊断工具在 [`诊断工具/`](诊断工具/)：`listen0310.bat`（旁听 PG14）、
`inject_h264.bat`（PC 冒充 Jetson 灌包）、`dumpcap.bat`（抓 MQTT）——
这三个**双击就能跑，不需要装 Python**。
`rm0310.py` 是拆 RoboGo 抓包的分析脚本，需要 Python（CH 这台 Windows 没有装，所以在别的机器上跑），
自带的 `listen0310_COM7_*.bin` 与 `H264镜头样本.bin` 是当时的原始抓包，可直接喂给它复算。

## 平台

| 项目 | 配置 |
|---|---|
| 主控 | STM32F407（RoboMaster 开发板 C 型） |
| 开发环境 | Keil MDK-ARM |
| 底盘类型 | **全向轮** |
| 赛季 | 26 赛季 |
| 代码版本 | **10.14 版本规则发布前** |

---

## 与自瞄的接口（重要）

### 三阶指令协议（32 字节帧）

本版本实现了「三阶指令」协议升级，是自瞄 `my_lf_INF_20261025` 所等待的配套版本。

**定义文件**：`gimbal_project/application/Self_aim.h`

```c
typedef struct {
    uint8_t fire_advicec;   // 0:不发 1:发(视觉决策)
    float   yaw;            // 云台 yaw 目标角 (度)
    float   yaw_vel;        // yaw 角速度 (度/秒)
    float   yaw_acc;        // yaw 角加速度 (度/秒²)
    float   pitch;          // pitch 目标角 (度)
    float   pitch_vel;      // pitch 角速度 (度/秒)
    float   pitch_acc;      // pitch 角加速度 (度/秒²)
    float   distance;       // 目标距离 (米)
} __attribute__((packed)) InputData;

#define RECEIVE_DATA_SIZE 32   // ← 旧版为 12
#define SEND_DATA_SIZE    19
#define FRAME_HEADER_FF  0xFF
#define FRAME_TAIL        0x0D
```

### 版本差异对照

| 项目 | 旧版（已废弃） | **本版本（正确）** |
|---|---|---|
| 接收帧长 | 12 字节 | **32 字节** |
| 字段 | fire / pitch / yaw / distance | **fire / yaw / yaw_vel / yaw_acc / pitch / pitch_vel / pitch_acc / distance** |
| `Self_aim.h` 大小 | 1579 字节（2026-03-10） | **2093 字节（2026-10-14）** |
| `CAN_receive.c` 大小 | 18534 字节（2026-02-27） | **19499 字节（2026-10-14）** |

> ⚠️ **必须使用本版本**。旧版 12 字节协议常量与 `autoaim/my_lf_INF_20261025` 的三阶指令管道**不兼容**。

### 自瞄侧对应配置

`autoaim/my_lf_INF_20261025/configs/` 中：

```yaml
three_order_enable: true    # 电控协议已就绪，可开启
```

开启后自瞄将发送 32 字节三阶帧，下位机 `Self_aim.c` 解析后用于前馈控制。

---

## 编译

### 环境要求

- Keil MDK-ARM（建议 5.x）
- STM32F4xx 器件支持包

### 步骤

1. 打开：
   - 底盘：`chassis_project/MDK-ARM/chassis_task.uvprojx`
   - 云台：`gimbal_project/MDK-ARM/standard_robot.uvprojx`
2. 编译（F7）
3. 编译无错误后由电控按既有流程处理；本次修复仅对应云台工程。

> **本仓库未包含编译中间产物**（`.o` / `.crf` / `.d` / `.axf` / `Objects` / `Listings` 等，
> 原目录约 350MB）。首次编译会全量重建，耗时较长属正常。

---

## 硬件说明

### 底盘（全向轮）

全向轮运动学与普通舵轮不同，`chassis_project/application` 中的解算需与实车轮胎布局一致。

### 云台

使用 C 板内置 IMU（BMI088）做姿态解算，电机控制环路参数在 `gimbal_project/application`。

---

## 相关文档

- 自瞄算法：`../autoaim/README.md`
- 自定义客户端：`../luojiamaster/使用说明.md`

---

## 说明

本代码为战队内部使用，仅用于技术交流。
