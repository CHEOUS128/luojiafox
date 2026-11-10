# luojiafox

珞珈狐战队 RoboMaster RM26 软件仓库。首页放本轮上车要做的事，换车、标定、组网和旧实测见[整车复现参考](docs/整车复现与历史实测.md)。

## 这次上车怎么测

本轮试验档选 **B**。它保留彩色和完整视野，画面是 `320×256`，目标每秒 20 张。在 Windows 仿真中，四轮一分钟测试的稳态最低是每秒 19 张，典型延迟约 0.1 秒。车上还要实际跑一遍。

| 顺序 | 现场做什么 | 看什么 |
| --- | --- | --- |
| 1 | 云台板刷入修复版，小电脑仍跑平时的服务，客户端看本车视频 | 车静止、不操作时能否持续出图。再单独测有操作时的表现 |
| 2 | 小电脑切到双包对照档 `baseline` | 看小电脑能否跟上，客户端收包和出图是否连续 |
| 3 | 小电脑切到 B 档 `quality` | 看移动时目标好不好认、停顿多久、掉图后能否恢复 |

同一段测试记下固件版本、Jetson 档位、车是否在动、操作手是否在操作。有异常时记下客户端日志的时间。现场操作客户端时避开 `Ctrl`，它会触发整车复位。

## 小电脑负责人

用 ToDesk 把这两个文件传到 Jetson 的 **`/home/nvidia4/cw_transfer/`**。打包文件都在本机仓库的 `compression-war/runs/delivery/`。

- `RM26_cw_trial_20261109_r2.zip`，8 个改动过的源码文件。
- `RM26_cw_trial_20261109_r2.sha256`，用来检查包有没有传错或损坏。

在这台 Windows 的 PowerShell 打开传输文件夹：

```powershell
Invoke-Item -LiteralPath 'C:\Users\CH\Desktop\robot记录\luojiafox\compression-war\runs\delivery'
```

Jetson 在独立目录 `~/my_lf_INF_20261025_cwtrial/` 编译，沿用本车的标定和模型。只编 `standard_mpc` 和 `video_sender_test`。两个程序的 `--help` 都出现 `cw-trial-20261109-r2`，就确认编到新版。

[一步步连 Jetson、传文件、编译、切到 B 和切回旧版](compression-war/docs/车端策略试验交付.md)。只想远程打开终端时用 ToDesk 就够；想让 Codex 从 Windows 读日志和检查编译结果，按同一页的局域网 SSH 步骤连。

## 电控负责人

拉取仓库 `main`，确认包含提交 **`722f2f8`**。这次改动都在 `lower-machine/26赛季全向轮步兵/gimbal_project/application/`：

| 文件 | 修了什么 |
| --- | --- |
| `Self_aim.c` | 视频帧校验失败后重新找下一帧的开头，避免后续一直读错位置。 |
| `video_forward.c` | 先把待发送的帧复制到独立缓冲，再发给图传，防止新数据在发送途中覆盖它。 |

用 Keil 打开[云台工程 `standard_robot.uvprojx`](lower-machine/26赛季全向轮步兵/gimbal_project/MDK-ARM/standard_robot.uvprojx)，选 Target **`standard_robot`**。编译看到 `0 Error(s)` 后，由电控按原流程烧到 **云台 C 板**，看到 `Programming Done` 再上车验证。

如果电控习惯单独选固件文件，取这次编译生成的 `gimbal_project/MDK-ARM/standard_tpye_c/standard_tpye_c.hex`。文件名里的 `tpye` 是工程原来的拼法。主机测试已通过，Keil 编译和实车结果由现场记录。[电控 README](lower-machine/README.md)

## 自定义客户端负责人

这次上车可直接双击 **`luojiamaster/程序/luojiamaster.exe`**。B 仍发客户端已经能解的 H.264。客户端两台电脑用同一份 EXE，按 `H` 看本车第二视角。

在这台 Windows 上从源码编译时，打开 PowerShell，整段复制：

```powershell
$src = 'C:\Users\CH\Desktop\robot记录\luojiafox\luojiamaster\源码'
$link = Get-Item -LiteralPath 'C:\rm26build' -ErrorAction Stop
if ($link.LinkType -ne 'Junction' -or $link.Target -ne $src) { throw 'C:\rm26build 指向不对，请先核对完整复现指南 3.4 节' }
Set-Location -LiteralPath $src
& '.\一键配置与编译.bat'
if ($LASTEXITCODE -ne 0) { throw '编译失败，查看源码目录的 cmake_build_log.txt' }
Get-Item -LiteralPath '.\build\release\luojiamaster.exe' | Select-Object LastWriteTime, Length
Get-FileHash -Algorithm MD5 -LiteralPath '.\build\release\luojiamaster.exe', '..\程序\luojiamaster.exe' | Format-List Hash, Path
```

这台电脑已用现有构建目录跑通脚本。产物大小为 **10,843,136 B**，MD5 是 **`f76e9fe71ca07a9f7be1ee1e7a543bfc`**，两份 EXE 完全相同。如果这次自己编出的值不同，先保留输出核对版本，再决定是否换 EXE。[从零编译和短路径设置](luojiamaster/完整复现指南.md)

## 四个目录

| 目录 | 用途 | 继续看 |
| --- | --- | --- |
| `autoaim/` | Jetson 自瞄和第二视角发送 | [自瞄 README](autoaim/README.md) |
| `lower-machine/` | 云台和底盘固件 | [电控 README](lower-machine/README.md) |
| `luojiamaster/` | Windows 客户端、接线和运行说明 | [使用说明](luojiamaster/使用说明.md) · [四台电脑现场手册](luojiamaster/现场施行手册-四台电脑-WiFi版.md) · [接手基线](luojiamaster/客户端能力基线.md) |
| `compression-war/` | Windows 上看片、比较和复核传输 | [仿真 README](compression-war/README.md) |

GitHub 留源码、说明、能复核结论的小样本和交付版客户端。`runs/` 里的重复试跑、传车 ZIP、编译日志和周报视频留在本机。过去的复现资料和参数集中在[整车复现参考](docs/整车复现与历史实测.md)，各子系统的过程记录在各自的 `档案/`。

自瞄基于[同济大学 SuperPower 战队自瞄算法](https://github.com/TongjiSuperPower)，客户端基于 ACE。沿用原项目许可。
