# 下位机代码（lower-machine）

> 珞珈狐战队 · RM26 赛季全向轮步兵固件
>
> **26 赛季全向轮步兵**的主控固件（STM32）。
>
> **版本：10.14 + 图传接收侧改造（2026-10-20）** —— 控制协议与 `autoaim/my_lf_INF_20261013` 匹配；
> 新增 0x0310 视频帧接收与转发（配合 `autoaim/my_lf_INF_20261025` 图传）。
>
> 📄 图传改造交接：[`图传接收侧改造说明.md`](图传接收侧改造说明.md)

---

## 目录

```
lower-machine/
└── 26赛季全向轮步兵/
    ├── chassis_project/     ← 底盘控制工程
    └── gimbal_project/      ← 云台控制工程
```

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

**需要确认**：VT03 接在哪个 UART（默认 `&huart6`，PG14 发）。
详见 [`图传接收侧改造说明.md`](图传接收侧改造说明.md)。

**当前问题：第二视角收不到画面**（控制正常）。排查用文档：

| 文档 | 用途 |
|---|---|
| **[`第二视角链路逐环节排查方案.md`](第二视角链路逐环节排查方案.md)** | **当前主文档**：逐跳隔离排查（灯 → 波形 → PC 扮演接收端 → PC 扮演 STM32 → 真链路计数），每条命令都有判读标准 |
| [`第二视角电控实施手册.md`](第二视角电控实施手册.md) | 零基础可照做，从原理到验收完整流程 |
| [`第二视角电控验收清单.md`](第二视角电控验收清单.md) | 可逐项打勾的快速检查表 |
| [`第二视角接线排查说明.md`](第二视角接线排查说明.md) | 接线原理与示波器判读细节 |
| **[`电控Debug入门手册.md`](电控Debug入门手册.md)** | **新手电控必读**：怎么区分证据与推测、三种调试手段各能证明什么、怎样给出能直接定位的结论 |
| **[`电控代码核对-2026-10-24.md`](电控代码核对-2026-10-24.md)** | **最新**：10-24 实测（发 683 帧、客户端收 0 条）+ 电控代码逐项核对 + UI 抢 USART6 DMA 的嫌疑与验证方法 |

排查工具在 [`诊断工具/`](诊断工具/)：`rm0310.py`（灌包 / 收包 / 拆 RoboGo 抓包 / MQTT 探针，带 `selftest`）。

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

本版本实现了「三阶指令」协议升级，是自瞄 `my_lf_INF_20261013` 所等待的配套版本。

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

> ⚠️ **必须使用本版本**。旧版 12 字节协议常量与 `autoaim/my_lf_INF_20261013` 的三阶指令管道**不兼容**。

### 自瞄侧对应配置

`autoaim/my_lf_INF_20261013/configs/` 中：

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
3. 烧录（F8）

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
