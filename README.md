# luojiafox

同济大学 SuperPower（罗家班）RoboMaster RM26 赛季软件集合。

## 目录结构

```
luojiafox/
├── luojiamaster/          # 自定义客户端（替代官方选手端）
├── autoaim/               # 自瞄算法（NVIDIA Jetson 端）
└── lower-machine/         # 下位机固件（STM32，26 赛季全向轮步兵）
```

---

## 各子系统说明

### 1. luojiamaster —— 自定义客户端

基于 [ACE_RM26_CustomClient](https://github.com/ACE-Algorithm/ACE_RM26_CustomClient) 二次开发。

**功能**：接收官方赛事引擎的比赛数据与机器人第一视角图传（HEVC 1080p60），并通过键鼠操控实车。

**前置条件（缺一不可）**：
1. 裁判端 `RoboMasterEngine` 已启动并登录
2. 官方选手端 `RoboMasterClient` **版本 ≥ 12.0.0.213**（关键）
3. 选手端图传画面正常显示
4. 选手端 P 面板 → 键鼠控制来源 → 选「自定义客户端」
5. luojiamaster 与选手端登录**同一机器人 ID**

**登录地址**：选手端同机 `127.0.0.1:3333`，异机 `选手端IP:3333`

**快捷键**：`Esc` 比赛模式 / `H` 视角切换 / `I` 画面翻转 / `P` 设置

详见 [`luojiamaster/完整复现指南.md`](luojiamaster/完整复现指南.md)。

---

### 2. autoaim —— 自瞄算法

> **当前版本针对 26 赛季全向轮步兵调校。**

基于 [同济大学 SuperPower 25 赛季自瞄](https://github.com/TongjiSuperPower) 二次开发。

**核心特性**：
- 双模型跟踪（装甲板 KF + 整车 EKF 并行）
- 三阶指令管道（角度 + 角速度 + 角加速度，备用）
- 无 ROS 依赖（核心算法）

**运行平台**：Jetson Orin，相机 1280×1024

**关键参数需按车标定**：`yaw_offset`、`pitch_offset`、弹速、相机内外参

详见 [`autoaim/README.md`](autoaim/README.md)。

---

### 3. lower-machine —— 下位机固件

> **26 赛季全向轮步兵。**

| 工程 | 职责 |
|---|---|
| `chassis_project` | 底盘运动控制、功率管理 |
| `gimbal_project` | 云台控制、发射机构、视觉通信 |

**开发环境**：Keil MDK-ARM，STM32

**与自瞄接口**：串口 16 字节自定义帧（帧头 `0xFF`，帧尾 `0x0D`，角度单位为度）

详见 [`lower-machine/README.md`](lower-machine/README.md)。

---

## 系统数据流

```
┌─────────────────┐  USB  ┌──────────────┐  串口  ┌──────────────┐
│ 相机 1280×1024  │──────►│ Jetson       │───────►│ STM32 云台   │
└─────────────────┘       │ (autoaim)    │  16B   │ (gimbal)     │
                          └──────────────┘        └──────────────┘
                                                         │ CAN
                                                         ▼
                                                  ┌──────────────┐
                                                  │ 电机 / IMU   │
                                                  └──────────────┘

┌─────────────────┐  MQTT  ┌──────────────┐  USB  ┌──────────────┐
│ 官方选手端      │───────►│ luojiamaster │───┐   │ VT13 接收端  │
│ (RoboMasterClient)      │ (PC)         │   │   └──────────────┘
└─────────────────┘        └──────────────┘   │
        ▲                                     │ 键鼠
        └─────────────────────────────────────┘
```

---

## 版本管理

| 子系统 | 当前版本 | 日期 |
|---|---|---|
| luojiamaster | v1.0 | 2026-10-17 |
| autoaim | my_lf_INF_20261013 | 2026-10-13 |
| lower-machine | 10.14 版本规则发布前 | 2025-10-14 |

---

## 已知限制

### luojiamaster
- 控制响应延迟高于官方选手端（多一跳 MQTT 转发）
- 视频为 CPU 软解，未启用硬件加速

### autoaim
- 仅针对 26 全向轮步兵调校，其他兵种需重新标定
- 双模型为「注入速度」混合做法，非完整实现
- 换板保护（`use_phase_jump_guard`）默认未启用

---

## 许可

各子系统沿用其上游项目 LICENSE，仅用于技术交流，不得商用。
