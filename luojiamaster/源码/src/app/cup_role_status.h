#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace RoboGo::app {

inline bool CupChassisStage(std::uint32_t stage) {
    // Preparation and referee self-check. Close submissions during the final countdown.
    return stage == 1 || stage == 2;
}

// UC 2027 protocol §2.2.7: event 15 is a public result, without a team or a robot ID.
inline std::optional<std::uint8_t> CupAssemblyResultCode(std::string_view param) {
    if (param.empty()) return std::nullopt;
    std::uint8_t value{};
    const auto end = param.data() + param.size();
    const auto parsed = std::from_chars(param.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
    return value;
}

inline std::string CupAssemblyResultName(std::uint8_t result) {
    constexpr std::array<const char *, 9> names{
        "成功", "拔出", "超时", "离开装配区过久", "重装战亡", "四级协作超时",
        "主动退出", "未检测到能量单元", "缓冲期结束"};
    if (result < names.size()) return names[result];
    return "未知（" + std::to_string(result) + "）";
}

inline std::string CupChassisName(std::uint32_t chassis) {
    switch (chassis) {
        case 1: return "血量优先";
        case 2: return "功率优先";
        default: return "未知（" + std::to_string(chassis) + "）";
    }
}

} // namespace RoboGo::app
