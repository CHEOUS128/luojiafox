# luojiamaster 自定义客户端

平时直接双击 [程序/luojiamaster.exe](程序/luojiamaster.exe)。这里统一维护当前版本、本次优化、实测结果和开发复核入口。

## 怎么用

1. 官方选手端先登录，确认官方图传正常，并将键鼠控制来源设为“自定义客户端”。
2. 本机连接官方端的网口设为 `192.168.12.2 / 255.255.255.0`，网关和 DNS 留空；对面官方端是 `192.168.12.1`。
3. 打开 EXE，服务器填 `192.168.12.1:3333`，选择与官方端一致的队伍和机器人，再登录。`Esc` 切换比赛模式，`H` 切换官方画面和本车第二视角。

**不要按 Ctrl，车端会把它当作整车复位。** 队友视角和完整按键见 [使用说明](使用说明.md)，四台电脑接线见 [现场手册](现场施行手册-四台电脑-WiFi版.md)。

## 当前版本

- 版本：2026-11-15 客户端性能优化版。
- 唯一交付程序：`程序/luojiamaster.exe`，与 `源码/build/release/luojiamaster.exe` 字节一致。
- 大小：**10,990,592 B**；编译时间：**2026-11-15 16:39:08（北京时间）**。
- MD5：`74a77a3d6bdf8e9dcab2b77976517ac7`。两台客户端更新后核对同一 MD5。
- 车端继续使用 [11-12 实车成功组合](../docs/2026-11-12-实车成功版本.md)；本轮更新只需更换 Windows 客户端。

## 本次优化与验证

官方主画面改用三槽对齐缓冲池，颜色转换直接写入可复用的图像内存，减少每帧一次整图复制。只有已经用完的缓冲才允许再次写入，显卡上传仍按实际行跨度复制。键鼠采样移到视频上传之前，原消息字段、鼠标累计方式和 13 ms 最短发送间隔保留。

| 本机测量 | 优化前 | 优化后 | 耗时减少 |
| --- | ---: | ---: | ---: |
| 1080p 图像转换，中位数 | 7.35 ms | 4.66 ms | 36.6% |
| 客户端内视频延迟，P95 | 68.99 ms | 65.02 ms | 5.8% |
| 普通负载下的本地输入响应，P95 | 18.22 ms | 18.06 ms | 0.9% |
| 人工压力下的本地输入响应，P95 | 20.26 ms | 11.01 ms | 45.7% |

人工压力档在每个界面帧额外加入 **8 ms 实际计算**。普通负载下的输入改善较小，压力档反映界面繁忙时提前采样的作用。画面颜色和分辨率保持原样，本轮短时对照中主、副画面实际更新仍约 **60 fps / 20 fps**。

![客户端优化对比](汇报图/客户端性能优化-汇报总图.png)

测量使用同一台 Windows、同一份录流、同一个带记录程序中的旧、新路径。图像转换每档统计 264 个预热后样本；视频每档 3 轮 × 120 秒，输入每档 3 轮 × 45 秒。视频和输入图中的曲线取三轮对应分位耗时的中位数，P95 表示 95% 位置的耗时。视频计时从本机收齐该帧码流到提交显示；模拟输入计时从事件进入到本地消息接收，车辆响应另行实测。

本轮已完成：5 项 CTest、6 项显卡故障恢复检查；118 张真实 1080p 图像转换及 1308 张第二视角输出的像素一致性回归通过。双路视频离线回放 **60 分钟未闪退**，预热后首尾内存均值增加约 **1.34 MiB**。

长时自动测试还有两处工具问题：模拟按键探针停在一个待匹配事件上；汇总文件漏写最终 GPU 等待次数。原组合验收保留 **3 项失败**，完整长测尚未通过。已完成的短时性能对照独立保留。下一步补齐自动长测，并上车复测登录、窗口放大、升级菜单和正常操控。

精确数字、各轮简表、源码与素材校验值、原失败项集中保留在 [精选验证数据](源码/tests/evidence/client-performance-20261115.json)。原始录流、逐帧 CSV、旧图、PDF 和临时构建留在维护电脑，仓库只保留当前这张汇报图。11-12 闪退修复的历史依据见 [稳定性记录](档案/2026-11-12-客户端视频稳定性修复.md)。

## 编译与离线复核

普通使用不用编译。开发者在仓库根目录打开 PowerShell；先检查 `C:\rm26build` 指向当前仓库的 `luojiamaster\源码`，再运行正式入口。编译环境和常见问题见 [完整复现指南第三节](完整复现指南.md#3-从源码编译)。

```powershell
$rm26Link = Get-Item -LiteralPath 'C:\rm26build' -ErrorAction Stop
$rm26Expected = (Join-Path (Get-Location).Path 'luojiamaster\源码').TrimEnd('\')
if ($rm26Link.LinkType -ne 'Junction' -or @($rm26Link.Target)[0].TrimEnd('\') -ine $rm26Expected) { throw 'C:\rm26build 指向不对，先修正目录链接。' }
& '.\luojiamaster\源码\一键配置与编译.bat'
if ($LASTEXITCODE -ne 0) { throw '客户端编译失败，请检查编译输出。' }
ctest --test-dir 'C:\rm26build\build\release' --output-on-failure --no-tests=error
if ($LASTEXITCODE -ne 0) { throw '测试未通过，先处理测试结果。' }
Copy-Item -LiteralPath '.\luojiamaster\源码\build\release\luojiamaster.exe' -Destination '.\luojiamaster\程序\luojiamaster.exe'
Get-FileHash -LiteralPath '.\luojiamaster\源码\build\release\luojiamaster.exe', '.\luojiamaster\程序\luojiamaster.exe' -Algorithm MD5
```

编译后，可用仓库自带的测试视频复核图像转换；输出 CSV 放到系统临时目录。这只测图像转换环节：

```powershell
& '.\luojiamaster\源码\build\release\real_conversion_benchmark.exe' '.\luojiamaster\程序\测试画面.h265' "$env:TEMP\rm26-rgba.csv"
```

双路回放需要维护人员保存的 `network_capture.bin`。在源码目录运行以下示例，替换录包和输出路径：

```powershell
.\build\release\luojiamaster.exe --offline-video-test --capture="D:\录包\network_capture.bin" --seconds=120 --replay-window-seconds=35 --performance --no-window-exercise --report-dir="D:\临时测试\新版"
```

加 `--performance-baseline` 比较旧图像路径和晚采样；再加 `--early-input` 可只比较采样位置。`--ui-work-ms=8` 为人工压力档。离线入口仅使用录包和本地消息接收函数，不连接车辆。性能数据由 `源码/tests/analyze_video_performance.py` 分析，长测由 `audit_long_video_performance.py` 检查，已知的长测记录问题按上文保留。

## 目录怎么找

| 位置 | 用途 |
| --- | --- |
| `程序/` | 唯一交付 EXE、正式启动和现场脚本、本地测试素材 |
| `源码/` | 客户端源码、构建配置、必要回归测试和精选验证数据 |
| `汇报图/` | 当前选定的一张优化对比图 |
| [使用说明](使用说明.md) | 登录、按键、队友视角和常见问题 |
| [客户端能力基线](客户端能力基线.md) | 三路视频、通信格式、输入与交接边界 |
| `档案/` | 已有历史依据；当前使用和本轮结果从本 README 开始 |
