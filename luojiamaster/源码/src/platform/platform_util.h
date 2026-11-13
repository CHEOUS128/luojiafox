#pragma once

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#endif

namespace RoboGo::platform_util {

inline std::filesystem::path PathFromUtf8(std::string_view value) {
#ifdef _WIN32
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("Invalid UTF-8 path");
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                           static_cast<int>(value.size()), wide.data(), size) != size)
        throw std::runtime_error("Cannot convert UTF-8 path");
    return std::filesystem::path(wide);
#else
    return std::filesystem::path(value);
#endif
}

inline std::string PathToUtf8(const std::filesystem::path &value) {
    const auto utf8 = value.u8string();
    return std::string(reinterpret_cast<const char *>(utf8.data()), utf8.size());
}

inline std::ifstream OpenBinaryInput(std::string_view path) {
    return std::ifstream(PathFromUtf8(path), std::ios::binary);
}

inline std::ofstream OpenUtf8Output(std::string_view path) {
    return std::ofstream(PathFromUtf8(path), std::ios::binary | std::ios::trunc);
}

inline std::filesystem::path ExecutableDirectory() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0 || size >= buffer.size()) throw std::runtime_error("Cannot resolve executable path");
    buffer.resize(size);
    return std::filesystem::path(buffer).parent_path();
#else
    return std::filesystem::current_path();
#endif
}

} // namespace RoboGo::platform_util
