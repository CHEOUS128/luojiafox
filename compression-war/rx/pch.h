// pch.h —— 仿真器专用的精简 PCH（本文件放在 rx/ 目录，靠 include 顺序优先于客户端的 pch.h）
//
// 客户端真实源码（luojiamaster 的 src/video/custom_byte_block_video_decoder.cpp）
// 第一行就是 `#include <pch.h>`。客户端的 pch.h 会拉进 boost / protobuf / async_mqtt /
// imgui / DX12，那套东西在 Linux/纯解码场景下一个都不需要，也编不过。
//
// 所以：把本目录（rx/）排在 include 搜索路径最前面，让这份文件**冒充** pch.h，
// 里面只保留解码器真正用到的 STL + spdlog + libav*。
//
// 注意：这不是修改客户端源码，只是换了一个更小的编译环境。
// 解码器源码本身仍然是仓库里那一份，一字未动。
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}
