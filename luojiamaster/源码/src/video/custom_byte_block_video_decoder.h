#pragma once

#include <pch.h>

namespace RoboGo::video {

struct CustomByteBlockVideoDecoderConfig {
    int max_reorder_frames = 8;
    std::size_t max_access_unit_bytes = 512U * 1024U;
    std::size_t max_assembly_bytes = 512U * 1024U;
    int max_frame_dimension = 4096;
    std::size_t max_frame_pixels = 4096U * 4096U;
};

struct CustomByteBlockDecodedFrame {
    std::uint32_t frame_id = 0;
    std::int64_t pts = 0;
    bool keyframe = false;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

class CustomByteBlockVideoDecoder {
public:
    struct Impl;

    explicit CustomByteBlockVideoDecoder(CustomByteBlockVideoDecoderConfig config = {});
    ~CustomByteBlockVideoDecoder();

    CustomByteBlockVideoDecoder(CustomByteBlockVideoDecoder &&) noexcept;
    CustomByteBlockVideoDecoder &operator=(CustomByteBlockVideoDecoder &&) noexcept;

    CustomByteBlockVideoDecoder(const CustomByteBlockVideoDecoder &) = delete;
    CustomByteBlockVideoDecoder &operator=(const CustomByteBlockVideoDecoder &) = delete;

    std::vector<CustomByteBlockDecodedFrame> push_packet_bytes(std::span<const std::uint8_t> wire_bytes);
    std::vector<CustomByteBlockDecodedFrame> flush();

private:
    CustomByteBlockVideoDecoderConfig config_;
    std::unique_ptr<Impl> impl_;
};

}// namespace RoboGo::video
