# luojiafox · 珞珈狐战队机器人软件

仓库有四个子系统。当前开发核心是 Windows 自定义客户端 **luojiamaster**。已有实车记录来自 2026 全向轮步兵；2027 联盟赛和超级对抗赛的界面可在 Windows 上本地测试。

## 第一次打开

解压后双击 [luojiamaster.exe](luojiamaster/程序/luojiamaster.exe)，选 **2027 高校联盟赛 → 本地测试 → 步兵 3 号**。点开始后按 **F11** 全屏，按 **M** 看全场，按 **Esc** 回到刚才的画面。

两个超级对抗赛入口也能按 M 看当年的场馆。本地测试里，两位队友的录像跟着样例车号移动；2027 指挥席另有三路录像和可旋转的三维场馆。正式入口的操作手大图目前只显示场地。

[三个入口怎么用](luojiamaster/README.md) · [新电脑部署](luojiamaster/部署教程/README.md) · [已实现功能和下一步](luojiamaster/部署教程/2027开发路线.md)

## 四个子系统各管什么

| 目录 | 用途 | 队员入口 |
| --- | --- | --- |
| `luojiamaster/` | Windows 比赛客户端、单机测试、队友画面和指挥席。支持接口匹配的不同车辆。 | [客户端使用说明](luojiamaster/README.md) · [部署教程](luojiamaster/部署教程/README.md) |
| `compression-war/` | Windows 压片仿真，在同一带宽条件下比较画质、延迟和帧率；保留双包基线、B 档和离线重放。 | [从实车样片开始仿真](compression-war/README.md) |
| `autoaim/` | 2026 全向轮步兵的 Jetson 参考实现。本车现役记录对应 `my_lf_INF_20261025`，另有早期版本。 | [自瞄与 Jetson 维护记录](autoaim/README.md) |
| `lower-machine/` | 2026 全向轮步兵的 Keil 云台与底盘参考工程，保存云台视频 DMA 转发的实车经验。 | [电控工程与实车记录](lower-machine/README.md) |

现有车辆和 2026 官方端复测用「2026 超级对抗赛 → 正式登录」。2026 年 11 月 12 日已确认工业相机画面经 Jetson、云台板、官方图传抵达 Windows 客户端。[当日实车记录](lower-machine/档案/2026-11-12-云台视频DMA修复与实车闭环.md)

## 维护约定

- **实车上不要按 Ctrl。串口数据接线不接 12V、VCC、5V、3V3 电源脚。** 官方网口和队内网口的用途见[工具与现场接线](luojiamaster/工具/README.md)。
- 车端改动先核对当前环境。车上文件由队员通过 ToDesk 手动传输，交接要写清文件、目的地和版本核对办法。Jetson 沿用参考车的两个指定编译目标，禁止全量 `make`。固件烧录由电控操作，先经 CH 确认。
- 客户端编译前核对 `C:\rm26build` 指向 `luojiamaster/源码`，构建后同步 `luojiamaster/程序/luojiamaster.exe` 并核对 MD5。压片仿真使用指向仓库根的 `C:\rm26cw`。
- Markdown 用 UTF-8 无 BOM 和 LF，PowerShell 用 UTF-8 BOM 和 CRLF。批处理保留原有 GBK 或纯 ASCII 和 CRLF。Windows 中文路径的文件访问沿用各项目的 `platform_util`。
- 操作说明进所属子系统的 README。客户端的排错经验合并进同一页，旧版逐次记录通过 Git 历史查。开发依赖、原始录像、场馆模型和个人电脑配置按各子系统说明保管。GitHub 提交与推送由 CH 操作。

自瞄基于同济大学 SuperPower 战队，客户端基于 ACE，各部分沿用其原许可。
