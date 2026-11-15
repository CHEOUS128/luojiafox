#pragma once

#include "app/application_types.h"

namespace RoboGo::video {

enum class RgbaConversionMode { LegacyCopy, ReuseAligned };

struct RgbaConversionSample {
    double convert_us = 0;
    double copy_us = 0;
    double total_us = 0;
    std::uint64_t copied_bytes = 0;
    bool allocation = false;
};

// The same converter is used by the production client and its isolated replay.
// LegacyCopy preserves the previous production conversion for controlled A/B tests.
// ReuseAligned writes directly to an exclusively owned, aligned buffer. Ownership
// remains with the VideoFrame until the renderer finishes its CPU upload copy.
class RgbaFrameConverter {
public:
    explicit RgbaFrameConverter(RgbaConversionMode mode = RgbaConversionMode::ReuseAligned);
    ~RgbaFrameConverter();
    RgbaFrameConverter(const RgbaFrameConverter &) = delete;
    RgbaFrameConverter &operator=(const RgbaFrameConverter &) = delete;
    std::optional<app::VideoFrame> Convert(const AVFrame *frame);
    [[nodiscard]] RgbaConversionSample LastSample() const { return last_; }
    [[nodiscard]] std::uint64_t PoolBusyCount() const { return pool_busy_; }
private:
    SwsContext *context_ = nullptr;
    AVFrame *legacy_frame_ = nullptr;
    RgbaConversionMode mode_;
    std::array<std::shared_ptr<std::vector<std::uint8_t>>, 3> pool_{};
    RgbaConversionSample last_{};
    std::uint64_t pool_busy_ = 0;
};

} // namespace RoboGo::video
