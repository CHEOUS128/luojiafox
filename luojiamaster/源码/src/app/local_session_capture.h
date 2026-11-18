#pragma once

#include <pch.h>
#include <span>

namespace RoboGo::app::local_capture {
using Bytes = std::span<const std::uint8_t>;
using Microseconds = std::chrono::microseconds;
struct Packet { std::uint64_t at; Bytes data; Bytes wire; };
struct Capture {
    std::array<std::vector<Packet>, 2> lanes;
    Microseconds period{};
};
inline std::uint64_t UnsignedLe(Bytes data, std::size_t offset, unsigned n) {
    std::uint64_t value = 0;
    for (unsigned j = 0; j < n; ++j) value |= std::uint64_t(data[offset + j]) << (8 * j);
    return value;
}
inline std::uint32_t RecordCrc(Bytes bytes) {
    static const auto table = [] {
        std::array<std::uint32_t,256> values{};
        for (std::uint32_t i = 0; i < values.size(); ++i) {
            auto v = i; for (int b = 0; b < 8; ++b) v = (v >> 1) ^ ((v & 1) ? 0xedb88320U : 0U);
            values[i] = v;
        }
        return values;
    }();
    std::uint32_t v = 0xffffffffU;
    for (auto c : bytes) v = table[(v ^ c) & 255] ^ (v >> 8);
    return ~v;
}
inline Capture ReadSample(Bytes bytes) {
    const auto bad = [] { throw std::runtime_error("内置双路录流不完整或校验失败"); };
    if (bytes.size() < 16 || std::memcmp(bytes.data(), "RBNCAP1\n", 8) != 0 || UnsignedLe(bytes, 8, 4) != 2) bad();
    bytes = bytes.subspan(16);
    Capture result;
    std::uint64_t first = UINT64_MAX, last = 0;
    while (!bytes.empty()) {
        if (bytes.size() < 12 || std::memcmp(bytes.data(), "REC2", 4) != 0) bad();
        const auto n = static_cast<std::size_t>(UnsignedLe(bytes, 4, 4));
        if (n < 18 || n > bytes.size() - 12) bad();
        const auto body = bytes.subspan(12,n);
        if (RecordCrc(body) != UnsignedLe(bytes,8,4) || body[9] != 1) bad();
        const auto ch_n = static_cast<std::size_t>(UnsignedLe(body,10,2));
        const auto off = 18 + ch_n + static_cast<std::size_t>(UnsignedLe(body,12,2));
        if (off > n || UnsignedLe(body,14,4) != n-off) bad();
        const std::string_view channel(reinterpret_cast<const char *>(body.data()+18),ch_n);
        Bytes payload = body.subspan(off);
        unsigned lane = 0;
        if (channel == "video-stream" && body[8] == 2) {
            if (payload.size() <= 8) bad();
        } else if (channel == "CustomByteBlock" && body[8] == 1) {
            // Field #1, protobuf length-delimited 300 B, in the fixed embedded fixture.
            if (payload.size() != 303 || payload[0] != 0x0a || payload[1] != 0xac || payload[2] != 0x02) bad();
            lane = 1; payload = payload.subspan(3);
        } else bad();
        const auto time = UnsignedLe(body,0,8);
        first = std::min(first,time); last = std::max(last,time);
        result.lanes[lane].push_back({time,payload,body.subspan(off)});
        bytes = bytes.subspan(12+n);
    }
    if (result.lanes[0].empty() || result.lanes[1].empty() || last-first > 60'000'000ULL) bad();
    for (auto &lane : result.lanes) {
        std::stable_sort(lane.begin(),lane.end(),[](const Packet &a,const Packet &b) { return a.at < b.at; });
        for (auto &p : lane) p.at -= first;
    }
    result.period = Microseconds(last-first+50'000); // Same explicit EOF padding as existing offline test.
    return result;
}

} // namespace RoboGo::app::local_capture
