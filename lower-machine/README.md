# 下位机代码（lower-machine）

> **26 赛季全向轮步兵**的主控固件（STM32）。

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

两个工程分别烧录到独立主控（或同一主控的不同任务）。

---

## 平台

| 项目 | 配置 |
|---|---|
| 主控 | STM32（RoboMaster 开发板） |
| 开发环境 | Keil MDK-ARM |
| 底盘类型 | **全向轮** |
| 赛季 | 26 赛季 |
| 代码版本 | 10.14 版本规则发布前 |

---

## 与自瞄的接口

自瞄（`autoaim/`）通过串口与云台主控通信，协议为**自定义 16 字节帧**：

```
帧头 0xFF ... 帧尾 0x0D
角度单位为「度」（不是弧度）
```

具体字段定义见 `gimbal_project` 中的串口解析代码。

> 注：`autoaim/my_lf_INF_20261013` 正在等待「三阶指令协议」（角度+角速度+角加速度，32 字节帧）
> 在电控侧落地后启用。

---

## 编译

### 环境要求

- Keil MDK-ARM（建议 5.x）
- STM32 对应芯片支持包
- 工程文件 `.uvprojx` 在 `MDK-ARM/` 目录

### 步骤

1. 打开 `MDK-ARM/chassis_task.uvprojx`（底盘）或 `MDK-ARM/gimbal_task.uvprojx`（云台）
2. 编译（F7）
3. 烧录（F8）

> **本仓库未包含 `MDK-ARM/` 编译产物目录**（`.o` / `.axf` / `Objects` / `Listings` 等，
> 原目录约 350MB）。请自行用 Keil 重新编译生成。

---

## 硬件说明

### 底盘（全向轮）

全向轮（麦克纳姆轮 / 全向轮）运动学与普通舵轮不同，
`chassis_project/application` 中的运动学解算需与实车轮胎布局一致。

### 云台

云台使用 C 板内置 IMU（BMI088）做姿态解算，
电机控制环路参数在 `gimbal_project/application` 中。

---

## 相关文档

- 自瞄算法：`../autoaim/README.md`
- 自定义客户端：`../luojiamaster/使用说明.md`

---

## 说明

本代码为战队内部使用，仅用于技术交流。
