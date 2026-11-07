@echo off
setlocal enabledelayedexpansion
chcp 936 >nul
cd /d "%~dp0"

REM ============================================================
REM  准备素材 -- 车端工业相机**原始彩色数据** -> 压缩链路输入
REM
REM  ★ 2026-11-07 CH 定的规矩（这条是本脚本的根）：
REM    原始数据是车上那台工业相机拍出来的，原封不动。彩色就是彩色。
REM    转灰度、缩放这些是"我们这边的处理"，不许作用在原始视频上。
REM    所以这里产出两路，用途严格分开：
REM
REM    素材\car_raw.bgr   相机原始分辨率 **彩色** BGR 紧排
REM                        -> 播放器左栏（原封不动）
REM                        -> 同时也是第 2 步的输入（处理只发生在第 2 步）
REM    素材\car192.gray   192x152 灰度（等比缩放 + 居中黑边 + 转灰度）
REM                        -> 真正喂给编码器的那一路
REM
REM    左栏 = car_raw.bgr（原片，先出现）
REM    右栏 = 编码+拆包+发出去+解码之后（处理完才出现）
REM    两栏的时间差就是延迟，播放器会实时显示。
REM
REM  用法：
REM    scripts\准备素材.bat "D:\video_test_raw.bin" 1280x1024
REM        ^<车端 video_sender_test -c 导出的原片^> ^<相机实际出图尺寸^>
REM        ★ 尺寸一定要填对：程序开头会打印 "相机实际出图 WxH"，照抄过来。
REM          填错会让第 1 步读出乱码画面（每帧字节数对不上）。
REM
REM    scripts\准备素材.bat "素材\car_raw.bgr" 1280x1024
REM        ^(ToDesk 传回来的原片直接改名成 素材\car_raw.bgr，跳过第 1 步不复制^)
REM
REM    scripts\准备素材.bat "D:\cam_raw\cam_raw"            ^(相机录的 PNG 目录^)
REM    scripts\准备素材.bat "D:\录像\实训场.mp4"            ^(视频 / 单个码流^)
REM        PNG/视频默认按 1280x1024 处理，源不是这个尺寸就补第 2 个参数。
REM
REM  ffmpeg.exe 的找法：先看本目录(scripts\)，再看 compression-war\，最后查 PATH
REM ============================================================

REM  ---- 定位 compression-war 根目录（scripts 的上一级） ----
set "HERE=%~dp0"
REM  Use cd below: it collapses dot-dot reliably.
cd /d "%~dp0.."
set "CW=%CD%"

set "SRCRAW=%~1"
set "SRC=%SRCRAW%"
set "RS=%~2"
if "%RS%"=="" set "RS=1280x1024"
REM  默认取 300 帧 = 车上 --seconds 15 @ 20fps；源不够就自动截到源的实际帧数。
set FRAMES=300

REM  ---- 拆出宽高（接受 1280x1024 / 1280X1024） ----
set "RSW="
set "RSH="
for /f "tokens=1,2 delims=xX" %%A in ("%RS%") do (
  set "RSW=%%A"
  set "RSH=%%B"
)
if "%RSW%"=="" goto badsize
if "%RSH%"=="" goto badsize
REM  ★ 必须先把数字单独提出来再 set /a：直接写 set /a PXW=%RSW%+0 时，
REM   cmd 先把 RSW 展开成 1280，变成 set /a PXW=1280+0，看起来没问题；
REM   但 RSW 一旦是空/'1280px' 这类东西，整行会变成 set /a PXW=+0 或语法错误，
REM   错误文本会漏到控制台。先提到纯数字变量再做算术。
set /a PXW=RSW+0 2>nul
set /a PXH=RSH+0 2>nul
if not defined PXW goto badsize
if %PXW% LEQ 0 goto badsize
if %PXH% LEQ 0 goto badsize
if not %PXW%==%RSW% goto badsize
if not %PXH%==%RSH% goto badsize
set /a RAWSZ=PXW*PXH*3
set /a GRAYSZ=192*152

REM  ---- 车端 VideoSender 的 letterbox 数值（必须和它逐像素一致）----
REM  video_sender.cpp:435-444 —— scale = min(192/W, 152/H)，等比缩到 NWxNH，
REM  再在 192x152 里居中补黑边，左 (192-NW)/2、上 (152-NH)/2 取整。
REM  1280x1024 -> scale 0.1484375 -> 190x152 -> 左 1 上 0。
REM  以前这里写死 190:152/pad 1:0，换个分辨率就全错，2026-11-07 改成现算。
REM  ★ cmd 的 if 只比整数、不认算式，乘积先算进变量；
REM    set /a 里也别写括号，块内容里多余的右括号会被当成块结束。
set /a LBW=PXW*152
set /a LBH=PXH*192
if %LBW% LEQ %LBH% (
  set /a NWT=PXW*152
  set /a NWT=NWT + PXH/2
  set /a NW=NWT/PXH
  set /a NH=152
) else (
  set /a NHT=PXH*192
  set /a NHT=NHT + PXW/2
  set /a NW=192
  set /a NH=NHT/PXW
)
set /a LEFT=192-NW
set /a LEFT=LEFT/2
set /a TOP=152-NH
set /a TOP=TOP/2

if "%SRCRAW%"=="" (
  echo [错误] 没给素材路径。
  echo.
  echo   用法：scripts\准备素材.bat "D:\video_test_raw.bin" 1280x1024
  echo         scripts\准备素材.bat "D:\cam_raw\cam_raw"
  echo         scripts\准备素材.bat "D:\录像\实训场.mp4"
  echo.
  pause
  exit /b 1
)
if not exist "%SRCRAW%" (
  echo [错误] 找不到素材：%SRCRAW%
  pause
  exit /b 1
)

REM  ---- 输入类型 ----
REM  .bin / .bgr / .raw = 裸帧（车端 video_sender_test 导出的就是这个），必须给 -s
set "IS_RAW=0"
if /i "%~x1"==".bin" set "IS_RAW=1"
if /i "%~x1"==".bgr" set "IS_RAW=1"
if /i "%~x1"==".raw" set "IS_RAW=1"

REM  目录 = 相机录的 PNG 序列，拼成 image2 的 %06d 模式
REM  ★ 别拿 "路径\." 或 "路径\*" 去猜是不是目录：cmd 对「文件 + 反斜杠」是宽松的，
REM    普通文件 car_raw.bgr\. 也会被判成存在，裸帧被当成 PNG 目录，
REM    报「目录里没有 000000.png」（2026-11-07 实踩）。
REM    这里直接从内容认：有 000000.png 就是 PNG 序列，否则按扩展名判裸帧。
if "%IS_RAW%"=="0" (
  if not exist "%SRCRAW%\000000.png" (
    echo [错误] 认不出这个素材：%SRCRAW%
    echo        裸帧文件必须叫 .bin / .bgr / .raw（大小写都行）；
    echo        相机 PNG 序列的目录里必须有 000000.png。
    pause
    exit /b 1
  )
  set "SRC=%SRCRAW%\%%06d.png"
)

REM  ---- 找 ffmpeg ----
set "FFM="
if exist "%HERE%ffmpeg.exe" set "FFM=%HERE%ffmpeg.exe"
if not defined FFM if exist "%CW%\ffmpeg.exe" set "FFM=%CW%\ffmpeg.exe"
if not defined FFM (
  for /f "delims=" %%P in ('where ffmpeg.exe 2^>nul') do if not defined FFM set "FFM=%%P"
)
if not defined FFM (
  echo [错误] 找不到 ffmpeg.exe。
  echo         放到这两个位置之一：%HERE%
  echo                            %CW%\
  echo         或者把它加进 PATH。
  pause
  exit /b 1
)

REM  ---- 输入参数 ----
set "FFM_IN="
if "%IS_RAW%"=="1" (
  set FFM_IN=-f rawvideo -pix_fmt bgr24 -s %RS% -r 30
) else (
  if /i not "%SRC%"=="%SRCRAW%" set FFM_IN=-start_number 0
)

mkdir "%CW%\素材" 2>nul

REM  源就是 素材\car_raw.bgr 本身时跳过第 1 步：ToDesk 传回来的 1.1GB 裸文件
REM  直接当原片用，省掉一次整盘复制。
set "SKIP1=0"
if /i "%~f1"=="%CW%\素材\car_raw.bgr" set "SKIP1=1"

if "%SKIP1%"=="1" (
  echo [提示] 源就是 素材\car_raw.bgr 本身，第 1 步跳过（不复制）。
  REM  顺手清掉以前留下的 temp_raw.bgr（30MB 垃圾，正常早该删）
  if exist "%CW%\素材\temp_raw.bgr" del /q "%CW%\素材\temp_raw.bgr"
  goto skip_step1
)

echo.
echo ============================================================
echo   第 1 步：相机原片 %RS% **彩色**（最多 %FRAMES% 帧）
echo ============================================================
echo   源  ：%SRCRAW%
if /i not "%SRC%"=="%SRCRAW%" echo   模式：%SRC%
echo   输出：%CW%\素材\car_raw.bgr   ^(左栏原片 + 第 2 步的输入^)
echo   每帧：%RSW% x %RSH% x 3 = %RAWSZ% 字节
echo.

REM  只做尺寸归一（源本来就是 %RS% 时 scale 是逐像素不变），**不裁剪、不转灰度**。
"%FFM%" -hide_banner -nostdin -y %FFM_IN% -i "%SRC%" -vsync 0 -frames:v %FRAMES% ^
  -vf "scale=%RSW%:%RSH%:flags=area" ^
  -pix_fmt bgr24 -f rawvideo "%CW%\素材\car_raw.bgr"

if errorlevel 1 (
  echo.
  echo [失败] 第 1 步出错。常见原因：
  echo   1. 第 2 个参数（尺寸）填错了 -- 第一个参数是 1280x1024，不是 192x152
  echo   2. 裸帧文件实际尺寸和 %RS% 不符 -- 看车端程序打印的「相机实际出图」
  echo   3. 源分辨率不足 / PNG 编号不连续 / 源太短
  echo.
  pause
  exit /b 1
)

:skip_step1

echo.
echo ============================================================
echo   第 2 步：编码链路输入 192x152 灰度（缩放+转灰度，这是"我们的处理"）
echo ============================================================
echo   %RSW%:%RSH% 等比缩放到 %NW%x%NH% 以内，居中补黑边凑成 192x152 = 左 %LEFT% / 上 %TOP%
echo   （和车端 VideoSender 的 keep_aspect letterbox 逐像素一致）
echo.

"%FFM%" -hide_banner -nostdin -y -f rawvideo -pix_fmt bgr24 -s %RS% -r 30 ^
  -i "%CW%\素材\car_raw.bgr" ^
  -vf "scale=%NW%:%NH%:flags=area,format=gray,pad=192:152:%LEFT%:%TOP%" ^
  -frames:v %FRAMES% -pix_fmt gray -f rawvideo "%CW%\素材\car192.gray"

if errorlevel 1 (
  echo [失败] 第 2 步出错。
  pause
  exit /b 1
)

REM  ---- 记下原片尺寸，跑仿真.bat 要读 ----
> "%CW%\素材\raw_size.txt" echo %RS%

echo.
echo ============================================================
echo   完成
echo ============================================================
echo.

REM ---- 校验：反推实际帧数（素材可能不足 FRAMES 帧）----
set SZ=0
for %%A in ("%CW%\素材\car192.gray") do set SZ=%%~zA
set RSZ=0
for %%A in ("%CW%\素材\car_raw.bgr") do set RSZ=%%~zA

set /a ACTUAL=%SZ% / %GRAYSZ%
set /a REM_BYTES=%SZ% %% %GRAYSZ%
if %RAWSZ% LEQ 0 (
  echo [错误] 算出来的单帧字节数是 0，尺寸 %RS% 有问题。
  pause
  exit /b 1
)
set /a RAF=%RSZ% / %RAWSZ%
set /a RREM=%RSZ% %% %RAWSZ%

echo   car_raw.bgr 字节数  = %RSZ%   （每帧 %RS%x3 = %RAWSZ%，共 %RAF% 帧）
echo   car192.gray 字节数  = %SZ%
echo   每帧字节数          = 192 x 152 = %GRAYSZ%
echo   实际帧数            = %ACTUAL%

if not "%RREM%"=="0" (
  echo.
  echo   [错误] car_raw.bgr 字节数不能被 %RAWSZ% 整除。
  echo          说明尺寸参数 %RS% 和文件里的实际分辨率不符。
  echo          把车端程序打印的「相机实际出图 WxH」原样作为第 2 个参数再跑一次。
  pause
  exit /b 1
)
if not "%REM_BYTES%"=="0" (
  echo.
  echo   [错误] car192.gray 字节数不能被 %GRAYSZ% 整除，分辨率不是 192x152。
  pause
  exit /b 1
)
if "%ACTUAL%"=="0" (
  echo.
  echo   [错误] 一帧都没裁出来。源可能太短或分辨率为空。
  pause
  exit /b 1
)
if not "%RAF%"=="%ACTUAL%" (
  echo.
  echo   [错误] 两路帧数不一致：car_raw %RAF% 帧 vs car192 %ACTUAL% 帧。
  echo          播放器左右会错位，先把这个数字发给我。
  pause
  exit /b 1
)

if not "%ACTUAL%"=="%FRAMES%" (
  echo   [提示] 少于目标 %FRAMES% 帧（源没那么长），不影响跑仿真。
)

echo.
echo   下一步：双击 scripts\跑仿真.bat      ^(可以带参数：码率 包率 每帧最多几包 编码耗时ms^)
echo.
pause
exit /b 0

:badsize
echo [错误] 第 2 个参数（原始分辨率）写得不对：%RS%
echo        正确写法：1280x1024
pause
exit /b 1
