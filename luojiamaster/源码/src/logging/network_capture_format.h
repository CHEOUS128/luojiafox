#pragma once

#include <pch.h>

namespace RoboGo::logging {

constexpr std::uint32_t kNetworkCaptureFlagInvertUdpVideo = 1U << 0U;

constexpr std::uint32_t BuildNetworkCaptureFlags(const bool invert_udp_video) {
    return invert_udp_video ? kNetworkCaptureFlagInvertUdpVideo : 0U;
}

}// namespace RoboGo::logging
