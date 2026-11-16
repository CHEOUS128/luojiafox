# luojiafox

珞珈狐战队机器人软件仓库，包含自瞄、下位机、Windows 客户端和压片仿真四个子系统。

**当前保留的是 2026 实车基础。** 本车已跑通 Jetson B 档视频、云台 DMA 转发和官方图传到自定义客户端。2027 RMUL 适配在当前版本整理、验稳之后进行；已有队友互看功能不代表已经完成 2027 官方接口适配。

后续方向已确定：UL、UC各有正式入口，底层共用，登录后按兵种自动切换对应界面，指挥手只在UC入口提供；待当前基础验收完成后实施。当前实际参赛目标是RMUL。

## 先怎么用

1. 按本车原接线给设备上电。Jetson 已有自启动，不要再手工开第二个自瞄程序。
2. 官方选手端先有画面，再按 `P` 把键鼠来源设为“自定义客户端”。客户端接官方端的网口用 `192.168.12.2/24`，网关和 DNS 留空。
3. 双击 `luojiamaster/程序/luojiamaster.exe`，连接 `192.168.12.1:3333`，选择相同队伍和机器人；登录后按 `Esc` 进入比赛模式，按 `H` 看工业相机第二视角。

第一次布置电脑，照 [客户端工具与现场接线](luojiamaster/工具/README.md) 做。普通操作和版本校验看 [客户端 README](luojiamaster/README.md)。

**不要按 Ctrl，它会触发整车复位。串口数据接线不接 12V、VCC、5V、3V3 电源脚。**

## 四个子系统分别看哪里

| 目录 | 管什么 | 入口 |
| --- | --- | --- |
| `autoaim/` | Jetson 的相机、自瞄、模型、标定、与云台通信，同时编码 B 档视频 | [自瞄与 Jetson 部署](autoaim/README.md) |
| `lower-machine/` | 云台 C 板、底盘固件；本车视频出口为云台 TX DMA | [电控工程与实车证据](lower-machine/README.md) |
| `luojiamaster/` | Windows 自定义客户端：本车画面、第二视角、键鼠和两台客户端互看 | [客户端使用与维护](luojiamaster/README.md) |
| `compression-war/` | 独立实时仿真、压缩策略对比与收包重放，不参与实车运行 | [压片仿真](compression-war/README.md) |

工业相机第二视角的当前链路为：

```text
相机 → Jetson 完整自瞄程序及 B 档编码 → USB
→ 云台 C 板校验、排队、DMA → VT03 → VT13
→ 官方选手端 → MQTT CustomByteBlock → 自定义客户端
```

现役 B 档为彩色、全视野、320×256、目标20 fps、H.264/GOP20。每包300 B数据段、最多296 B码流、309 B外层；所有图像含关键帧最多2包/592 B，最高50包/秒。具体服务与维护命令只维护在自瞄 README，云台修改及2391包实测只维护在电控 README。

## 维护约定

- 先查实际运行目录、服务、配置、模型和依赖，再改车端。车端文件仍通过 ToDesk 手动传，写明“传什么 → 传到哪 → 怎么确认新版本”。
- Jetson 只编 `standard_mpc`、`video_sender_test`，不跑全量 `make`；云台固件由电控按配套 Keil 工程处理，烧录须经 CH 确认。
- 改客户端后同步唯一交付 EXE，两台客户端 MD5 一致；编译前确认 `C:\rm26build` 指向 `luojiamaster/源码`。仿真使用 `C:\rm26cw` 指向仓库根。
- `.md` 为 UTF-8 无 BOM+LF；`.ps1` 为 UTF-8 BOM+CRLF；`.bat` 按原 GBK+CRLF 或明确的 ASCII+CRLF 编译入口维护。中文路径文件操作使用各自 `platform_util`。
- 日志、完整录流、临时构建、重复图表和传输包留在维护电脑。仓库保留必要测试素材、小份证据及各子系统档案。历史正文不改，旧结论的更正在档案索引顶部追加。
- GitHub 的 `add`、`commit`、`push` 由 CH 在 Windows 执行。

自瞄基于同济大学 SuperPower 战队，客户端基于 ACE；各部分沿用其原许可。
