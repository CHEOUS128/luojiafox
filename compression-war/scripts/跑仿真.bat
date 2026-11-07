@echo off
setlocal enabledelayedexpansion
chcp 936 >nul
cd /d "%~dp0"

REM ============================================================
REM  压片战争 -- 一键跑一遍
REM
REM  用法：
REM    跑仿真.bat                       实车同款（码率自动 / 40 包每秒 / 编码 51ms / 120 帧）
REM    跑仿真.bat 74000                 指定码率
REM    跑仿真.bat 60000 40 8            码率 包率 每帧最多几包
REM    跑仿真.bat 60000 40 8 51 300     再加第 5 个：最多跑多少帧（默认 120）
REM
REM  五个位置参数：码率 / 每秒包数 / 一帧最多几包 / 每帧编码耗时(ms) / 最多几帧
REM    编码耗时默认 51 = 车上 2026-11-07 17:16 实测（15.125 秒编出 293 帧）。
REM    给 0 = 只算链路、不算编码（学习用；播放器会标注"未计编码"）。
REM
REM  和实车的关系（★ 仿真的默认值就是照着车上那台抄的）：
REM    码率 0        = 和车端 standard3.yaml 一样，走同一条公式（≈59.6 kbps）
REM    编码线程数    = 不给 --threads，x264 自己开 = 车端 ctx-^>thread_count = 0
REM    编码耗时 51   = 车端独立编码线程实测 19.4 帧/秒
REM    → 相机 300 帧时，仿真也会因为编码线程跟不上丢掉 9 帧（车端实测 293/302）
REM    → 右栏在这些地方停住，和实车上看到的卡顿是同一个原因
REM
REM  要做 A/B、要每次跑得一模一样：命令行手动加 --threads 1（脚本不再默认钉死）。
REM
REM  前提：素材已经准备好（见 README 第二节），素材和脚本都在
REM  compression-war\ 下，脚本放在 scripts\。
REM
REM  cw_sim.exe 的查找顺序：本目录(scripts\) → compression-war\ → PATH
REM
REM  素材必须是「车端相机原片」导出的两路：
REM    素材\car_raw.bgr   相机原始分辨率彩色原片（只给播放器左栏，不参与编码/统计）
REM    素材\car192.gray   192x152 灰度（编码器真正吃的）
REM ============================================================

REM  ---- 定位 compression-war 根目录（scripts 的上一级） ----
set "HERE=%~dp0"
REM  Use cd below: it collapses dot-dot reliably.
cd /d "%~dp0.."
set "CW=%CD%"

REM  第 1 个参数 = 码率（bps）。默认 0 = 自动，和车端 standard3.yaml 的 bitrate 一样走同一条公式
REM  （≈59.6 kbps）；给具体数字就硬覆盖，不受桶上限夹取。
set "BITRATE=0"
if not "%~1"=="" set "BITRATE=%~1"
REM  第 2 个参数 = 每秒包数（第 3 个 = 一帧最多几个包）
set "SR=40"
if not "%~2"=="" set "SR=%~2"
set "PKT=8"
if not "%~3"=="" set "PKT=%~3"
REM  第 4 个参数 = 每帧编码耗时（毫秒）。0 = 不算编码（右栏只显示链路延迟）
REM  默认 51 = 车上 2026-11-07 17:16 实测：15.125 秒编出 293 帧（19.4 帧/秒）。
REM  注意：这个值不是估计，是那轮日志算出来的；换车/换分辨率/换画质档要重测。
set "ENCMS=51"
if not "%~4"=="" set "ENCMS=%~4"
REM  第 5 个参数 = 最多跑多少帧（默认 120 = 6 秒）。
REM  播放器会把每帧左栏原片都写成 1280x1024 的 PNG；300 帧时浏览器要同时
REM  扛 300 张大图 + 280 张右图，容易卡。120 帧足够看出压缩差异和延迟。
set "NFMAX=120"
if not "%~5"=="" set "NFMAX=%~5"

REM  ---- 比赛硬约束：RM 2026 通信协议 V2.0.0 的 0x0310 ----
REM  数据段 300 字节、频率上限 50Hz。300 x 50 = 120 kbps 是空口天花板。
REM  超了仿真器会拒绝，这里提前拦一道，省得白跑一遭。
if %SR% GTR 50 (
  echo [错误] 包率 %SR% Hz 超过协议 0x0310 的 50Hz 上限，仿真不能跑。
  pause
  exit /b 1
)

REM  ---- 找 cw_sim.exe ----
set "SIM="
if exist "%HERE%cw_sim.exe" set "SIM=%HERE%cw_sim.exe"
if not defined SIM if exist "%CW%\cw_sim.exe" set "SIM=%CW%\cw_sim.exe"
if not defined SIM (
  for /f "delims=" %%P in ('where cw_sim.exe 2^>nul') do if not defined SIM set "SIM=%%P"
)

REM  ---- 前置检查 ----
set MISSING=0
if not defined SIM           ( echo [错误] 缺 cw_sim.exe，先跑 scripts\一键编译.bat & set MISSING=1 )
if not exist "%CW%\素材\car192.gray" ( echo [错误] 缺 compression-war\素材\car192.gray，先跑 scripts\准备素材.bat & set MISSING=1 )
if not exist "%CW%\素材\car_raw.bgr" (
  echo [错误] 缺 素材\car_raw.bgr（相机原片，左栏要它）。
  echo        先跑 scripts\准备素材.bat 把车端 video_test_raw.bin 转过来。
  set MISSING=1
)
if "%MISSING%"=="1" (
  echo.
  echo.
  echo 先补齐缺的东西，说明见 README 第二节和第三节。
  pause
  exit /b 1
)

REM  ---- 读原片尺寸（准备素材.bat 会写进 raw_size.txt）----
set "RS=1280x1024"
if exist "%CW%\素材\raw_size.txt" set /p RS=<"%CW%\素材\raw_size.txt"
if "%RS%"=="" set "RS=1280x1024"

REM ---- 用素材实际帧数跑（素材可能只有 120 帧，不是 200）----
set SZ=0
for %%A in ("%CW%\素材\car192.gray") do set SZ=%%~zA
set /a NFRAMES=%SZ% / 29184
set "NFALL=%NFRAMES%"
if %NFRAMES% GTR %NFMAX% set /a NFRAMES=%NFMAX%
if "%NFRAMES%"=="0" (
  echo [错误] 素材文件是空的，先跑 scripts\准备素材.bat。
  pause
  exit /b 1
)

set OUT=%CW%\runs\%BITRATE%
set "BRTXT=%BITRATE% bps"
if "%BITRATE%"=="0" (
  set "OUT=%CW%\runs\auto"
  set "BRTXT=自动（和车端同一条公式 ≈59.6kbps）"
)
echo.
echo ============================================================
echo   压片战争  码率 %BRTXT% / %SR% 包每秒(上限50) / 20 fps / 跑 %NFRAMES% 帧（素材共 %NFALL% 帧）
echo   左=相机原片(彩色,未处理, %RS%)   右=缩放+转灰度+编码+拆包+发送+解码   编码耗时 %ENCMS% ms/帧
echo   编码线程=x264 自动(和车端 ctx-^>thread_count=0 一致)
echo   输入 %CW%\素材\car192.gray
echo   输出 %OUT%
echo ============================================================
echo.

REM  --ref 是播放器左栏用的：相机原片，%RS% **彩色**，同一取景、原封不动。
REM  转灰度/缩放都是"我们这边的处理"，只出现在右栏，绝不作用在原片；
REM  它只影响显示，不参与编码/封包/统计。尺寸错了会读出乱码，所以走 raw_size.txt。
set "REF="
if exist "%CW%\素材\car_raw.bgr" set REF=--ref "%CW%\素材\car_raw.bgr" --ref-pix bgr24 --ref-size %RS%

"%SIM%" --input "%CW%\素材\car192.gray" --width 192 --height 152 --pix gray ^
  --fps 20 --frames %NFRAMES% --out "%OUT%" --bitrate %BITRATE% ^
  --send-rate %SR% --max-pkts %PKT% --encode-ms %ENCMS% %REF%

if errorlevel 1 (
  echo.
  echo [失败] cw_sim 返回错误，看上面的提示。
  pause
  exit /b 1
)

echo.
echo 读数说明：
echo   SSIM-Y          = 右栏相对左栏（原片）的亮度相似度
echo   需求包率        = Σceil(帧字节/296) / 时长，这是"要多少包"，可能超过 100%%
echo   右栏的「延迟」  = 这一帧处理完才出现的时刻 - 它被拍到的时刻（播放器时间轴上实时显示）
echo   编码丢帧        = 相机拍到、但编码线程还没取走就被下一帧覆盖（从没进过 x264）
echo                     编码耗时 ^>= 50ms 时每 50 帧左右丢 1 帧；右栏会在那一刻停住
echo                     明细在 per_frame.csv，why 列写 encode-drop
echo.
echo   左栏 = 车上工业相机的原始彩色画面，原封不动。
echo   右栏 = 同一帧走完 缩放 → 转灰度 → H.264 → 300B 拆包 → 令牌桶 → 客户端解码 之后的样子。
echo.

REM  ---- 打开对比播放器 ----
set "PLAYER=%OUT%\viewer\player.html"
if exist "%PLAYER%" (
  echo 正在打开对比播放器...
  start "" "%PLAYER%"
) else (
  echo [注意] 没找到播放器，可能是导出失败了，看上面的 warn。
)

echo.
echo 跑完了。结果在 %OUT%\summary.txt 和 %OUT%\per_frame.csv
echo.
pause
