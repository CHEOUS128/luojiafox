#pragma once

#include <pch.h>

namespace RoboGo::logging {

void initialize(std::string_view application_name, bool invert_udp_video, bool enable_network_capture = true);

void shutdown();

bool is_initialized();

const std::filesystem::path &log_root_directory();

const std::filesystem::path &log_directory();

const std::filesystem::path &network_capture_path();

const std::string &session_id();

std::string truncate_text(std::string_view text, std::size_t max_length = 512);

void log_network_payload(std::string_view transport,
                         std::string_view direction,
                         std::string_view channel,
                         std::span<const uint8_t> payload,
                         std::string_view peer = {});

// 抓包队列满而被丢弃的记录数（抓包线程死掉时会持续增长）。
std::size_t network_capture_dropped_count();

inline void log_network_payload(std::string_view transport,
                                std::string_view direction,
                                std::string_view channel,
                                std::string_view payload,
                                std::string_view peer = {}) {
    const auto *begin = reinterpret_cast<const uint8_t *>(payload.data());
    log_network_payload(transport,
                        direction,
                        channel,
                        std::span<const uint8_t>(begin, payload.size()),
                        peer);
}

}// namespace RoboGo::logging
