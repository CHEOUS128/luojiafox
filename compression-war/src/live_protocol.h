#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>
#include <string>
namespace cw::live {
inline constexpr std::size_t kPacketBytes=300;
inline constexpr std::size_t kPayloadBytes=296;
inline constexpr int kFramePackets=2;
inline constexpr std::size_t kFrameBytes=kPayloadBytes*kFramePackets;
inline constexpr int kVbvBits=static_cast<int>(kFrameBytes*8);
inline constexpr double kMaxFps=50.0/kFramePackets;
inline constexpr int kMinimumDisplayFps=15;
using Transport = std::array<std::uint8_t, kPacketBytes>;
using Outer = std::array<std::uint8_t, 309>;
std::uint8_t crc8(std::span<const std::uint8_t> bytes);
std::uint16_t crc16(std::span<const std::uint8_t> bytes);
Outer wrap(const Transport &data, std::uint8_t seq);
bool unwrap(std::span<const std::uint8_t> frame, Transport &data);
std::vector<std::uint8_t> protobuf(const Transport &data);
bool unprotobuf(std::span<const std::uint8_t> bytes, Transport &data);
// Hash only VCL NALs: parser grouping of SPS/PPS must not affect trace matching.
std::uint64_t vcl_hash(std::span<const std::uint8_t> au);
struct Fit { int x, y, width, height; };
Fit fit(int source_w, int source_h, int target_w, int target_h);
std::string pixel_sha256(std::span<const std::uint8_t> rgba);
std::string fingerprint_line(std::uint64_t ordinal,std::uint32_t frame_id,std::int64_t pts,
                             int width,int height,std::span<const std::uint8_t> rgba);
} // namespace cw::live
