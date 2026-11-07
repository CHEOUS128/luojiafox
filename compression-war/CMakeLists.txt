# compression-war 仿真器
#
# 依赖的三块东西：
#   1. 客户端的真实解码器源码（从 ../luojiamaster/源码 拿，不复制、不修改）
#   2. 客户端自带的那份 ffmpeg 静态库（只解不编，编码走外部 ffmpeg 进程）
#   3. spdlog / stb 头文件（同样来自 luojiamaster/源码/3rdparty_downloads）
#
# Linux 构建：
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#
# Windows 构建：先跑 脚本\一键编译.bat（会进 VS 开发者环境）
cmake_minimum_required(VERSION 3.20)
project(cw_sim CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- 仓库内依赖的根目录 ----
set(LJ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../luojiamaster/源码" CACHE PATH "luojiamaster 源码根目录")

if(NOT EXISTS "${LJ_ROOT}/src/video/custom_byte_block_video_decoder.cpp")
  message(FATAL_ERROR
    "找不到客户端解码器源码：${LJ_ROOT}/src/video/custom_byte_block_video_decoder.cpp\n"
    "compression-war 必须放在 luojiafox 仓库里（和 luojiamaster 同级）。")
endif()

set(FFMPEG_DIR "${LJ_ROOT}/3rdparty/ffmpeg")
set(SPDLOG_DIR "${LJ_ROOT}/3rdparty_downloads/spdlog/include")
set(STB_DIR    "${LJ_ROOT}/3rdparty_downloads/stb")

if(WIN32)
  set(FFMPEG_LIBS "${FFMPEG_DIR}/lib/windows_x86_64/avcodec.lib"
                  "${FFMPEG_DIR}/lib/windows_x86_64/swscale.lib"
                  "${FFMPEG_DIR}/lib/windows_x86_64/avutil.lib")
else()
  set(FFMPEG_LIBS "${FFMPEG_DIR}/lib/linux_x86_64/libavcodec.a"
                  "${FFMPEG_DIR}/lib/linux_x86_64/libswscale.a"
                  "${FFMPEG_DIR}/lib/linux_x86_64/libavutil.a")
endif()

# ---- 客户端解码器：作为独立目标编译，用 rx/pch.h 顶替客户端的重 PCH ----
add_library(rm_decoder STATIC
  "${LJ_ROOT}/src/video/custom_byte_block_video_decoder.cpp")

target_include_directories(rm_decoder
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/rx"          # 关键：让 rx/pch.h 优先于客户端 pch.h
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${LJ_ROOT}/include"
    "${LJ_ROOT}/src"
    "${FFMPEG_DIR}/include"
  PRIVATE
    "${SPDLOG_DIR}")

target_link_libraries(rm_decoder PUBLIC ${FFMPEG_LIBS})
target_compile_options(rm_decoder PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-w>)
# 客户端源码里有大量中文注释；MSVC 默认按 GBK 读，会报 C2001/C2015 之类的错。
# 客户端工程自己也加了这一条（luojiamaster/源码/CMakeLists.txt 第 11 行）。
if(MSVC)
  target_compile_options(rm_decoder PRIVATE /utf-8 /bigobj)
endif()

# ---- 仿真器本体 ----
add_executable(cw_sim
  src/main.cpp
  src/sim_runner.cpp
  src/encoder.cpp
  src/packetizer.cpp
  src/raw_io.cpp
  src/metrics.cpp
  src/viewer.cpp
  src/platform_util.cpp)

# ⚠ 顺序有讲究：rm_decoder 的 INTERFACE 里带 rx/（冒充 pch.h），
#   而这里又写了 ${LJ_ROOT}/include（里面有客户端的重 pch.h，会拉 boost）。
#   CMake 把「目标的直接 include 目录」排在「依赖目标传播的目录」之前，
#   所以 sim_runner.cpp 会先命中原版 pch.h → boost/asio.hpp not found。
#   把 rx/ 显式放第一，压过它。
target_include_directories(cw_sim PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/rx"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${STB_DIR}"
  "${SPDLOG_DIR}"
  "${FFMPEG_DIR}/include")

# Windows 上没有 pthread 这个库：MSVC 会报 cannot open input file 'pthread.lib'。
# 客户端解码器用的是 std::thread/condition_variable，Windows 上本来就不需要额外库。
if(WIN32)
  target_link_libraries(cw_sim PRIVATE rm_decoder ${FFMPEG_LIBS})
  # platform_util 用到了 CommandLineToArgvW。它有三种链接方式：
  #   shell32.lib / onecore_shell.lib / onecore.lib —— 不同 Windows SDK 提供的不一样。
  # 不写死某个 .lib：直接问 SDK 哪个存在，用第一个存在的。
  find_library(CW_SHELLLIB NAMES shell32 onecore_shell onecore)
  if(CW_SHELLLIB)
    target_link_libraries(cw_sim PRIVATE ${CW_SHELLLIB})
  endif()
  # ffmpeg 静态库本身没有把系统依赖写进 .lib，MSVC 只能靠最终链接时补。
  # 客户端的 ffmpeg 打包只给了三个 .lib，这里把通常需要的系统库一次列全；
  # 用不到的库对链接没有副作用。
  target_link_libraries(cw_sim PRIVATE
    ws2_32 bcrypt secur32 user32 ole32 strmiids mfplat mfuuid shlwapi advapi32)
else()
  target_link_libraries(cw_sim PRIVATE rm_decoder ${FFMPEG_LIBS} pthread)
endif()

# ffmpeg.exe 的默认路径：Windows 下优先用仓库里同目录的 ffmpeg.exe，
# 找不到就退回 PATH。可以在命令行用 --ffmpeg 覆盖。
if(WIN32)
  target_compile_definitions(cw_sim PRIVATE CW_FFMPEG_EXE="ffmpeg.exe")

  # 源码里有大量中文注释，统一按 UTF-8 读进来（客户端工程也是这么设的）。
  # ⚠ 前提是源文件本身确实是 UTF-8。这个仓库里的源文件都是，别用会改编码的编辑器去存。
  target_compile_options(cw_sim PRIVATE /utf-8)
  target_compile_options(rm_decoder PRIVATE /utf-8)

  # 崩溃时给个能看懂的调用栈，而不是只报个地址。
  # 二进制会大一点，但对这种「只在自己机器上跑」的工具完全值得。
  target_compile_options(cw_sim PRIVATE /Zi)
  target_link_options(cw_sim PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()
