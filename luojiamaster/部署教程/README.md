# 自定义客户端部署教程

这份教程面向第一次下载程序的队员，以及准备让其他车辆使用本客户端的算法、电控队员。客户端是当前仓库的主要开发方向，按接口接收画面、状态和发送操作，**不依赖全向轮这种底盘结构**。仓库中的26赛季全向轮步兵，是已经跑通过的参考车；它的自瞄参数、串口协议和整车固件不能直接当作所有车型的配置。

这里讲跨电脑、跨车型怎么接入，以及移植时必须保留的改法。日常按键、当前EXE版本和验证结果看[客户端README](../README.md)，详细网卡接线看[工具README](../工具/README.md)。现有车辆继续使用 2026 正式入口。Windows 已有两个 2027 赛事入口，联盟赛优先开发，现场接入需等官方端联调。

第一次只想试画面的队员可以从下面第 2 节开始。已经装好、想按本节点验收的队员看[换一台电脑怎么试](2027开发路线.md#下一步先让新队员在另一台电脑独立用起来)。

首页已有三个赛事。2027 超级对抗赛的指挥席目前可在一台 Windows 电脑本地测试；两份三维地图为可选文件。正式指挥手登录、四机指挥席及双屏列在[已实现与待实现的路线](2027开发路线.md)。

## 1 先确定你需要部署到哪一步

| 需要什么 | 要准备什么 | 车端是否需要修改 |
| --- | --- | --- |
| 在一台 Windows 上试操作手界面或指挥席 | EXE 已带二维地图和实录，三维场馆另带对应模型 | 只用本机 |
| 在Windows看官方主画面、比赛数据并操作本车 | 客户端、官方选手端、匹配的机器人身份与控制接口 | 原车已有官方接入时，先核对兼容性；不要求换成本仓库自瞄或整车固件 |
| 在Windows互看队友的官方主画面 | 两到三台客户端、各自的官方链路和队友专用有线网 | 登录同一赛事、同一阵营的不同岗位后自动配对 |
| 增加工业相机第二视角 | 相机取图、Jetson视频发送模块、控制板转发、官方图传、客户端 | 需要按第4～6节接入额外视频通道 |
| 在电脑比较压缩方案 | 独立的compression-war仿真 | 不需要接车；仿真不参与正式链路运行 |

本仓库四个部分的现状：

| 子系统 | 当前是什么 | 换车时怎么用 |
| --- | --- | --- |
| [自瞄代码](../../autoaim/README.md) | `autoaim/my_lf_INF_20261025`为本车Jetson完整自瞄及B档视频；`20261013`为旧参考 | 完整自瞄移植要核模型、相机标定、云台协议和依赖；只要视频时可复用发送模块 |
| [电控代码](../../lower-machine/README.md) | `lower-machine/26赛季全向轮步兵`中的Keil云台、底盘工程；云台已加入DMA视频转发并实车成功 | 本车可用配套工程；其他车在自己的工程中移植视频收发，保留自己的电机、IMU和控制逻辑 |
| [压片仿真](../../compression-war/README.md) | 独立实时仿真和策略对比；已选B档作为本车视频起点 | 用来比较方案、验证封包与限速；其C板旁路、理想通道不能证明真实转发已正常 |
| [自定义客户端](../README.md) | 当前开发核心，独立Windows画面与操作端 | 接口匹配后可接其他车辆；不要求对方使用全向轮或相同自瞄算法 |

## 2 新电脑从GitHub拿什么 怎么运行

### 2.1 推荐完整下载仓库

打开[GitHub仓库](https://github.com/CHEOUS128/luojiafox)，点 **Code → Download ZIP**，完整解压；也可以在装有Git的新电脑PowerShell中执行：

```powershell
$rmRepo = Join-Path $env:USERPROFILE 'Desktop\luojiafox'
git clone https://github.com/CHEOUS128/luojiafox.git $rmRepo
```

这条命令用于首次下载，目标文件夹应尚不存在。已有工作副本在确认自己的改动已保存后更新：

```powershell
$rmRepo = Join-Path $env:USERPROFILE 'Desktop\luojiafox'
git -C $rmRepo pull --ff-only
```

下载不等于要安装四套程序。普通操作手只运行 `luojiamaster/程序/luojiamaster.exe`，不用编译，不用在Windows安装Jetson环境。

### 2.2 电脑需要什么

- Windows 10/11 64位，显卡支持DirectX 12。
- 报缺少 `MSVCP140.dll`、`VCRUNTIME140.dll` 或 `VCRUNTIME140_1.dll` 时，安装Microsoft Visual C++ x64运行库。
- EXE 内置 FFmpeg 解码库、字体、二维地图和本地测试录像。普通运行无需另装 FFmpeg、Unity 或编译器。

| 新电脑要看什么 | 带哪些文件 | 打开后的效果 |
| --- | --- | --- |
| 操作手比赛界面、指挥席二维地图和三路录像 | 下载后从可写目录运行 `程序/luojiamaster.exe` | 录像、中文字体和二维底图都在 EXE 里。刚从 GitHub 解压就可以选「本地测试」 |
| 指挥席三维场馆 | 从队内已有场馆包的电脑复制 `程序/arena/` 到新电脑 EXE 旁，同目录保留模型来源说明 | `light.mesh` 对应精简三维，`detailed.mesh` 对应精细三维。缺少所选模型时显示二维，三路录像按选中的布局继续播放 |

三维场馆文件由队内保管，GitHub 下载包只包含生成工具和来源说明。本机日志和队友专线设置在各自电脑生成。更新后可在 PowerShell 核对手里的 EXE，输出应和[客户端版本表](../README.md#当前版本和验证范围)一致。在仓库根目录运行：

```powershell
(Get-FileHash -LiteralPath '.\luojiamaster\程序\luojiamaster.exe' -Algorithm MD5).Hash
```

<details><summary>维护时如何从原模型重新制作地图包</summary>

从 hxyulin 的 `rmuc2027-simplified-glb-20261018` 发布包取出 `field.glb`。精细版从掉分员 Unity 工程的 Git LFS 资源取 `field_visual_m.fbx`。两者都基于 2027 超级对抗赛场馆。转换工具会校验原文件的 SHA256、三角面数量，并保留地面、坡道和高地的比例。二维地图取自 2027 超级对抗赛比赛规则手册 V1.0.0 图 4-1。 [精简场馆](https://github.com/hxyulin/rm-map-tools/releases/tag/rmuc2027-simplified-glb-20261018) · [精细场馆](https://github.com/diaofenyuan/RMUC_2027map_unity) · [官方图纸](https://bbs.robomaster.com/article/1970956?source=4)

在 `luojiamaster` 目录打开 PowerShell，把资源路径换成实际位置。使用 Python 3.12。先输出到新文件夹，确定后再替换本机已有地图。

```powershell
py -3.12 -m pip install numpy Pillow assimp_py==1.2.0
py -3.12 .\工具\内部\生成指挥席场馆.py light .\field.glb .\新场馆\light.mesh
py -3.12 .\工具\内部\生成指挥席场馆.py detailed .\field_visual_m.fbx .\新场馆\detailed.mesh
```

</details>

需要维护工具、离线测试时，最省事的是保留完整的 `luojiamaster` 目录。精简分发至少保留下面的相对位置：

```text
luojiamaster/
├─ 程序/
│  ├─ luojiamaster.exe
│  └─ arena/                 可选的本机三维场馆
├─ 工具/
│  ├─ 维护工具.bat
│  ├─ 离线测试.bat
│  └─ 内部/                 两个入口对应的PS1文件
└─ 源码/tests/fixtures/
   ├─ 双路视频样本.bin
   └─ 双路视频样本.json      样本来源与校验记录
```

双路离线回放读取 `双路视频样本.bin`。`测试画面.h265` 供图像转换测试和维护人员重制官方主画面样片使用；普通队员运行 EXE 时不用另外找录像。源码编译见[开发者编译与离线复核](../工具/README.md#开发者编译与离线复核)。

### 2.3 先做无车检查

1. **看 3V3 比赛画面。** 选「2027 高校联盟赛 → 本地测试 → 红方步兵 3 号 → 开始测试」，按 F11。应该同时看见本车主副视频、控制度条、六根血条和联盟赛地图。按 H 换主视角，再按 M 打开大地图。按 Esc 应直接回到刚才的视角。再按 L 看队友、按 L 返回。换蓝方后，大地图应和右下角小图同向。
2. **看 7V7 操作手。** 选 2027 超级对抗赛的本地测试、蓝方步兵 3。按 F11、M，分别点蓝 1 和蓝 4 的随车视频，再按 Esc 回到本车。2026 用自己的底图，1 号叫英雄。正式 M 大图只显示场地。
3. **看 7V7 指挥席。** 选「2027 超级对抗赛 → 本地测试 → 指挥手 → 开始测试」，按 F11。默认在地图上看三路随车小窗。单击一路就地放大，点另一路切换，再点一次缩回。顶部选「左右分栏」可恢复原布局。在地图上左键挪、右键转、滚轮放大，两种布局可当场对照。
4. **检查缺模型时的显示。** 只有 EXE 的新电脑，左边应有二维底图、样例位置和模型提示；二维可平移、缩放。需要三维时补齐对应的模型后重开程序。现有参考车实测选择「2026 超级对抗赛 → 正式登录」。

更多岗位、测试赛况和单路断开的操作见[客户端说明](../README.md)。

双击 `工具/离线测试.bat`，选择内置样本，约20秒后结束。它只读本机录包，不创建正常客户端网络服务、不向车辆发送键鼠。应看到官方HEVC和300 B第二视角两路出画，工具最后显示完成和双路出画为True。

这一步检查电脑上的解码、显示及文件路径；两台独立回放窗口不能证明队友网络已经接通。两台操作电脑最终使用相同EXE，校验值以[当前版本表](../README.md#当前版本和验证范围)为准。

## 3 新车接官方主画面和键鼠

### 3.1 接车前先与电控对清这三项

1. **身份匹配。** 2026 登录页支持红方 `1/2/3/4/6`、蓝方 `101/102/103/104/106`，标签仍为旧赛季。目标身份或当年字段不在当前支持范围时，先适配客户端，不能选另一个ID代替。
2. **键鼠匹配。** 原车应已按官方链路正确接收键鼠；对清本车按键、鼠标方向和模式。参考车固件把Ctrl作为整车复位，本教程沿用“不按Ctrl”的操作约定。
3. **私有数据匹配。** 当前客户端还会在官方状态和所需血量数据齐全后，每100 ms自动发送本队13 B `CustomControl`：5个小端u16敌方血量，加3个状态/计数字节。这路不以Esc为开关。新车如果把自定义数据用于机械臂或其他机构，先由电控核对并完成兼容处理，再登录联调；只确认标准键鼠能用还不够。实际格式见[载荷定义](../源码/src/custom_control_payload.h)和[发送实现](../源码/src/main_window.cpp)。

这三项不要求对方更换整套自瞄或固件，要求的是双方收发含义一致。

### 3.2 先把本车这一对接通

```text
本车官方图传 → 官方选手端电脑
官方端192.168.12.1 ──独立网线── 客户端电脑192.168.12.2
```

两端专用网口掩码 `255.255.255.0`，网关、DNS留空。官方端先登录、先正常显示本车画面；在官方端按P，把键鼠来源设为“自定义客户端”。

打开EXE，服务端填 `192.168.12.1:3333`，选择与官方端一致的队伍和机器人，登录后确认主画面、比赛数据正确。由现场人员确认执行器条件后，按Esc进入比赛模式检查本车操作。这里的Esc控制键鼠采样；上一节的13 B自动上行另行核对。

当前基础通过官方选手端接入，MQTT数据用3333端口，官方HEVC视频用UDP3334。详细设置和排错见[现场接线](../工具/README.md#单车先接通官方端)。

队友互看使用另一块有线网卡。两台可直连，三台接同一交换机，分别设为 `10.77.0.1/24`、`.2`、`.3`，网关留空。登录同赛事、同阵营的不同岗位后自动互看官方主画面。原来的 A/B 分配已取消，电脑地址和岗位彼此独立。专线及防火墙设置见[队友接线](../工具/README.md#四台电脑的当前摆法)。

## 4 增加工业相机第二视角 先统一通道约定

```text
自家相机已取到的BGR图
→ Jetson缩小/编码/封包/逐包发送
→ USB → C板拆帧/校验/排队/DMA整包发送
→ VT03 → VT13 → 官方选手端
→ MQTT CustomByteBlock → 本客户端H.264解码 → 按H查看
```

双方先固定下面这一套，之后再讨论更改画质或策略：

| 项目 | 当前成功B档 |
| --- | --- |
| 图像 | 彩色、完整视野，等比缩小补边至320×256，目标20 fps |
| 编码 | H.264 Annex-B，GOP20，约74.5 kbps，VBV4736 bit；保留SPS/PPS和图像数据 |
| 每个数据包 | 300 B：2 B大端长度、2 B大端内层序号、最多296 B码流，余下补0；长度值含2 B序号、不含自身2 B |
| 每张图 | 包括关键帧在内，最多2包、592 B净码流；超限整帧丢弃并恢复关键帧，不截断、不加第三包 |
| 外层 | 5 B帧头＋2 B命令字0x0310＋300 B数据＋2 B CRC16，共309 B；帧头含CRC8 |
| 发送节拍 | 最高50包/秒；本车发送器在一包写完后至少再等20 ms，调度晚了不突发补发 |
| 序号 | 外层8位、内层16位，分别处理回绕 |

官方主画面的HEVC与这路H.264是两种码流。C板原样转发309 B包，不负责压缩、解码、分图或补颜色。不要拿PNG导出文件大小当成通信载荷大小。

## 5 Jetson该拿哪些文件 接到自己的什么地方

参考源码统一位于 `autoaim/my_lf_INF_20261025/`，下面的文件链接可以直接打开：

| 文件 | 作用与接入位置 |
| --- | --- |
| [video_sender.hpp](../../autoaim/my_lf_INF_20261025/io/video_sender/video_sender.hpp)、[video_sender.cpp](../../autoaim/my_lf_INF_20261025/io/video_sender/video_sender.cpp) | 创建视频发送器，将自家已取得的BGR图交给 `feed(img, timestamp_ns)`；负责缩小、H.264和协议封包 |
| [cw_trial.hpp](../../autoaim/my_lf_INF_20261025/io/video_sender/cw_trial.hpp) | `quality`档、双包预算、去除SEI说明及逐包节拍约定 |
| [cw_tx_worker.hpp](../../autoaim/my_lf_INF_20261025/io/video_sender/cw_tx_worker.hpp) | 后台取 `take_trial_frame()`；交给自家串口发送管理器，并持续服务未完成的写入 |
| [gimbal.hpp](../../autoaim/my_lf_INF_20261025/io/gimbal/gimbal.hpp)、[gimbal.cpp](../../autoaim/my_lf_INF_20261025/io/gimbal/gimbal.cpp) | 本车控制协议与串口适配参考；重点复用写锁、半帧续写和帧边界，其他车保留自己的控制字段 |
| [standard_mpc.cpp](../../autoaim/my_lf_INF_20261025/src/standard_mpc.cpp) | 本车完整自瞄与视频共存的接入示例；不是独立纯视频程序 |
| [video_sender_test.cpp](../../autoaim/my_lf_INF_20261025/tests/video_sender_test.cpp) | 合成图或相机本地编码检查，输出包文件与时间记录，不向C板串口发送 |

**只加第二视角：** 使用上表的4份视频模块 `video_sender.hpp/.cpp`、`cw_trial.hpp`、`cw_tx_worker.hpp`，并接好[crc.hpp](../../autoaim/my_lf_INF_20261025/tools/crc.hpp)、[crc.cpp](../../autoaim/my_lf_INF_20261025/tools/crc.cpp)、日志和构建依赖；复用自家已有取图与唯一串口管理器。视频模块需要OpenCV、FFmpeg的avcodec/swscale/avutil以及可用的libx264编码器；它调用库，不调用外部ffmpeg进程。按自家工程加入源文件与链接库。

**连自瞄一起迁移：** 需要完整工程、相机SDK、模型和配置，按目标车重做内参、手眼关系、云台坐标及单位、弹速与弹道偏移。本车 `standard3.yaml` 是示例，不照抄其中的物理参数。具体标定与本车依赖见[自瞄README](../../autoaim/README.md#换车必须重做的标定-最重要)。

### 5.1 更新已有本车工程与部署新工程要区分

[源码打包器](../../compression-war/tools/package_vehicle_trial.py)只包含上表实际8份文件、逐文件清单和校验值，是**已有对应工程的更新包**。它没有完整CMake、模型、相机SDK或本车标定，不能当作一套新Jetson安装包。

| 场景 | 传什么 → 传到哪 → 如何确认 |
| --- | --- |
| 更新已有同框架工程 | 新的8文件源码ZIP及同名.sha256 → ToDesk手动传到目标用户Downloads，再按包内相对路径更新已核对的工程目录 → 核ZIP及包内SHA，编译后核帮助、时间、大小 |
| 新Jetson部署完整自瞄 | 完整源码、目标机依赖、该车模型与配置 → ToDesk手动传到事先确定的新目录，保留原程序 → 完成目标机编译、模型/标定核验及服务入口核验 |
| 其他自瞄框架只加视频 | 视频模块及依赖、已审查的取图/串口适配改动 → ToDesk手动传到对方确认的工程相对位置 → 对比源码SHA，先本地封包检查，再联调真实出口 |

不把ARM依赖或TensorRT engine当作Windows文件使用，也不把同名engine当作跨设备通用模型。先看目标机CUDA、OpenCV、相机SDK和 `ldd` 结果。

本车生成更新包和目标机编译的完整步骤只维护在[Jetson维护说明](../../autoaim/README.md#以后维护或换电脑)。已准备好本车构建目录后，只编指定目标：

```bash
# 本车示例路径；其他车换成已核对的工程和构建目录
cd /home/nvidia4/my_lf_INF_20261025_cwtrial
make -C build_cw standard_mpc video_sender_test -j2
./build_cw/standard_mpc --help | grep 'cw-trial-20261109-r2'
./build_cw/video_sender_test --help | grep 'cw-trial-20261109-r2'
stat -c '%y %s %n' build_cw/standard_mpc build_cw/video_sender_test
sha256sum build_cw/standard_mpc build_cw/video_sender_test
ldd build_cw/standard_mpc
```

本车生产入口必须显式带 `--cw-profile=quality`；`video_sender_test`的写法为 `--cw-profile quality`。不带档位仍按旧YAML流程，不能因为源码里有B档就认为已经启用。

## 6 电控该拿哪个工程 其他车怎么移植

### 6.1 使用本仓库这辆26全向轮步兵

电控取配套完整云台工程，Keil打开 [standard_robot.uvprojx](../../lower-machine/26赛季全向轮步兵/gimbal_project/MDK-ARM/standard_robot.uvprojx)，Target选 `standard_robot`。输出为：

```text
lower-machine/26赛季全向轮步兵/gimbal_project/MDK-ARM/standard_tpye_c/standard_tpye_c.hex
```

目录实际拼写是 `tpye`。原配置为STM32F407IGHx、ARM Compiler 5.06 update 7 build 960；由电控编译、按原流程烧录云台C板，烧录仍须负责人确认。本次视频修复不需要因此重烧底盘，当前构建标记 `VideoForward_Debug.build_tag` 为 `0x261112D1`。

### 6.2 其他车保留自己的整车工程 接入下面这些环节

参考文件统一在 `lower-machine/26赛季全向轮步兵/gimbal_project/`。本模块含STM32F4寄存器操作，移植时按目标板适配；只改一个串口句柄不等于完成跨芯片移植。

| 必须接好什么 | 参考文件与函数 | 其他车怎么改 |
| --- | --- | --- |
| USB真实字节入口 | [Src/usbd_cdc_if.c](../../lower-machine/26赛季全向轮步兵/gimbal_project/Src/usbd_cdc_if.c) → `usbd_cdc_receive_handle` | 传入本次实际收到的长度；USB回调可能是半包，也可能含多包 |
| 控制与视频拆帧 | [application/Self_aim.c](../../lower-machine/26赛季全向轮步兵/gimbal_project/application/Self_aim.c) 的 `process_usb_data_packet` | 保留一套状态机；本车区分0xFF的32 B控制和0xA5的309 B视频，自家控制协议不同就适配这一层 |
| 视频校验与队列 | [video_forward.c](../../lower-machine/26赛季全向轮步兵/gimbal_project/application/video_forward.c)、[video_forward.h](../../lower-machine/26赛季全向轮步兵/gimbal_project/application/video_forward.h) | 成套加入工程，完整包交给 `VideoForward_PushFrame`，检长度、命令字及双CRC后入队 |
| 初始化与持续服务 | `Self_aim_task`；[Src/freertos.c](../../lower-machine/26赛季全向轮步兵/gimbal_project/Src/freertos.c) | 调一次 `VideoForward_Init`；实际创建的任务每5 ms运行 `VideoForward_Task`，不能等键鼠输入才调用 |
| UART和TX DMA | [Src/usart.c](../../lower-machine/26赛季全向轮步兵/gimbal_project/Src/usart.c)、[Src/dma.c](../../lower-machine/26赛季全向轮步兵/gimbal_project/Src/dma.c) | 本车USART6、921600/8N1，PG14 TX、PG9 RX；TX为DMA2_Stream6/Channel5普通模式，绑定hdmatx。其他板按硬件重配 |
| DMA完成中断 | [Src/stm32f4xx_it.c](../../lower-machine/26赛季全向轮步兵/gimbal_project/Src/stm32f4xx_it.c) | 当前 `DMA2_Stream6_IRQHandler` 调 `HAL_DMA_IRQHandler(&hdma_usart6_tx)`；新工程接对应IRQ |
| 最后一个字节真正发完 | [application/remote_control.c](../../lower-machine/26赛季全向轮步兵/gimbal_project/application/remote_control.c) | 在USART IRQ原接收逻辑之外接 `VideoForward_TxCompleteFromISR`，保留原遥控接收DMA和IDLE处理 |
| 启动与工程包含 | [Src/main.c](../../lower-machine/26赛季全向轮步兵/gimbal_project/Src/main.c)、Keil工程 | UART/DMA/USB先初始化，相关源文件与IRQ确实参与编译；保持与本板时钟、DWT配置一致 |

最后一次DMA修复只改了 `video_forward.c/h` 和 `remote_control.c`，因为上述其他接入当时已经存在。**新车从零移植，不能因此只拷这三个文件。** 电控完成移植后编译自家云台/通信板工程，核对本次生成文件，再安排烧录与静止验证。

## 7 必须带走的经验 防止换车后重新踩坑

### 7.1 C板看见包头 不等于整包已经发出

本车曾经静止时很难出图，移动后偶尔出一帧。实抓C板出口时，有2317个包头，但完整309 B、双CRC通过的包是0；片段中位数278 B，771组都把同一前缀重复三次。旧代码用20 ms阻塞发送，失败后从头再试两次；原HAL主机测试复现了 `278 × 3`。

已经确认的直接问题是**发送不完整，随后从包头重试**。修复采用非阻塞DMA，并保留下面五件事：

1. 队列包复制到独立发送缓冲；等UART TC确认整包发完，或确认TX DMA已停止后，才能复用缓冲。仅DMA搬运完成还不算整包发送完成。
2. DMA搬完内存后还要等UART TC，最后一字节真正发完才计成功；DMA IRQ和UART TC两段都要接。
3. 当前旧HAL的 `HAL_UART_Transmit_DMA` 未完整检查底层启动结果，移植采用本模块的启动检查、状态机和计数，不能只替换一个API名称。
4. 发送异常只恢复TX；保留遥控RX DMA、IDLE和接收状态，不做全串口Abort。`HAL_BUSY`表示这次未启动，不能抢改其他发送者状态；`HAL_TIMEOUT`可能已经发出前半包，不能记整包成功。确认硬件停不下来时保留缓冲，报告故障。
5. C板每次实际发送启动，包括重试，至少间隔20 ms；DWT不可用时保守等21个HAL tick，迟到不突发补发。任务每轮有限工作，100 ms是异常看门狗，不是阻塞等待时间。

同条件静止复测60.014秒，得到2391个完整包，CRC错、缺号、重复均为0，之后接回官方图传，用户确认客户端正常出画。完整证据、34项主机用例和后续两轮采集Overrun都保留在[DMA实车闭环](../../lower-machine/档案/2026-11-12-云台视频DMA修复与实车闭环.md)。没有实测板上具体抢占时长，不能把“CPU怎样忙”写成已经量出的原因；这里验的是完整包和修复效果。

### 7.2 Jetson的write返回部分长度 不能丢尾或从头重发

早期串口库零超时会短写：309 B可能只交出一部分，旧代码把它当发送结束，后续控制字节又被C板当成视频尾部吞掉。历史PTY测试100次写入有32次短写，控制帧仍能偶尔正常，并不能排除这个问题。

移植保留同一把写锁和字节偏移：**已开始的帧先补完 → 控制帧 → 新视频包**。控制帧短写也从断点继续，不能整条重发；新视频不能覆盖正在续写的包。若write异常且无法知道已经写出多少，停止后续写入并报错，不能猜断点或从包头重试。`push=true`只是本地接管，不等于整包已写入、更不等于客户端已收到。[历史证据](../../autoaim/档案/图传v3-串口写修复-2026-10-24.md)中的旧单包参数不作为当前B档参数。

### 7.3 没有新图供给 先查相机和姿态等待

本车还曾因 `gimbal.q()` 等不到下位机状态，导致主循环与视频供给一起停住；重新接好下位机后恢复。当前完整入口的实际顺序仍是“取图 → 等姿态 → 自瞄 → 发控制 → feed视频”。相机自己在出图，不代表视频编码器正在收到新图；先看主循环、姿态及成帧日志。[历史判读](../../autoaim/档案/主循环卡顿诊断-2026-10-23.md)只作经验参考，旧目录和旧参数不照跑。

只移植视频时接自家已取得的图，避免把整套姿态和自瞄等待也搬过去。`feed()`持有的是 `cv::Mat`浅拷贝；相机SDK若复用原缓冲，接入处要保有独立图像或正确管理缓冲寿命，例如把 `img.clone()` 交给feed；不能因为feed已返回，就重新写入后台仍在读取的内存。

### 7.4 新源码 新二进制 新服务 是三件事

- B档必须显式选择 `quality`，它会覆盖普通视频配置；YAML仍显示灰度/8包、旧注释写40 Hz时，不能拿它替代实际档位和日志。
- `--help`看见 `cw-trial-20261109-r2`，只说明这个文件包含新入口；还要用 `systemctl show/cat`确认服务真正运行的路径、参数、覆盖配置。
- 本车现役目录是 `/home/nvidia4/my_lf_INF_20261025_cwtrial`，服务是 `autoaim-20261025.service`。其他车先确认自己的目录、服务和设备，不能把这些绝对路径照填过去。
- 一台相机和一条USB由现役程序管理，不另开第二个完整自瞄或视频进程抢设备。
- 本车仍依赖旧目录 `/home/nvidia4/my_sp_main/io/daheng/lib/armv8/liblog4cplus_gx.so`，清理前保留；新机用 `ldd`核自己的真实依赖。模型、标定、SDK库不跟8文件更新包一起覆盖。
- `video_sender_test`成功只说明本地编码/封包检查成功，它不会向C板发串口。仿真也旁路C板，二者都不能代替C板出口验证。

## 8 逐段验证 找到第一处不同再处理

先保持车辆静止，现场电控确认执行器状态。需要改车端文件时，先列明“传什么 → 传到哪 → 如何确认”，通过ToDesk手动传输；固件烧录另行确认。

| 顺序 | 检查什么 | 通过的依据 |
| --- | --- | --- |
| 1 电脑离线 | 解码、显示、文件路径 | 内置20秒双路回放完成；这一步不接车 |
| 2 官方主链路 | 官方端、自定义客户端的本车画面、身份与原车操作 | 官方端先正常；核私有13 B兼容后，再验客户端登录和实际操作 |
| 3 Jetson供给与USB写入 | 相机、姿态、编码、完整写入计数 | 主循环及成帧持续；完整写入计数也持续，而不只看“已编码” |
| 4 C板出口 | 309 B帧长度、双CRC、内外序号和数据 | USB字节/入队/启动/TC完成分开看；静止时也应持续产生完整合法包 |
| 5 官方转发与解码 | MQTT收到的300 B、解码和实际新图显示 | 对齐C板包中 `[7:307]` 的300 B与接收载荷，再看解码数和显示数 |
| 6 原功能与恢复 | 自瞄、键鼠、断流恢复、重连、正常开机启动 | 本车原功能仍可用；缺包后能等关键帧恢复；确认实际服务为本次版本 |

以序号和载荷字节/摘要逐段对应；不同电脑未同步的时钟不直接用来计算端到端延迟。收到包数、解码帧数、成功显示新图数分别统计，旧画面停住时不能当作持续出画。

如需只旁听串口，由电控核对板图、实际TX口和波特率；参考车出口是PG14，CH340仅接RXD/GND，TXD悬空。12V、VCC、5V、3V3都不接，也不向C板发送键鼠。参考车实测串口为921600/8N1，不凭“UART6”等板上文字直接猜接线。

本教程完成的是可复现的部署和移植路径；当前EXE的已测范围、短测内存观察项与实车待复测内容，始终以[当前版本与验证记录](../README.md#当前版本和验证范围)为准。新车联调记录只报告实际完成项，源代码相同、编译成功或看见包头，都不单独等于整条链路验收完成。
