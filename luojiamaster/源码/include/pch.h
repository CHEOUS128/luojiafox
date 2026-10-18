#pragma once

// this remove unused-includes warning in clangd
// see: https://github.com/include-what-you-use/include-what-you-use/blob/master/docs/IWYUPragmas.md#iwyu-pragma-begin_exportsend_exports
// IWYU pragma: begin_exports

// STL
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
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
// spdlog
#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
// asio
#define BOOST_ASIO_NO_DEPRECATED
#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
// protobuf
#include <google/protobuf/message.h>
#include <messages.pb.h>
// async_mqtt
#include <async_mqtt/all.hpp>
// ffmpeg
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}
// readerwriterqueue
#include <readerwriterqueue.h>
// stb
#define STBI_ONLY_PNG
#include <stb_image.h>
#ifdef _WIN32
// windows
#include <windows.h>
#include <shellapi.h>
// dx12
#include <d3d12.h>
#include <dxgidebug.h>
#include <dxgi1_5.h>
#else
// glfw
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#endif
// imgui
#include "imgui.h"
#ifdef _WIN32
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#else
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#endif
#include "imgui_internal.h"
#include "imgui_stdlib.h"

// IWYU pragma: end_exports
