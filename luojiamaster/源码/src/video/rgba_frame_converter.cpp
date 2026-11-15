#include "rgba_frame_converter.h"

namespace RoboGo::video {
namespace {
using Clock = std::chrono::steady_clock;
double Micros(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::micro>(end - begin).count();
}
}

RgbaFrameConverter::RgbaFrameConverter(RgbaConversionMode mode) : mode_(mode) {
    if (mode_ == RgbaConversionMode::LegacyCopy) {
        legacy_frame_ = av_frame_alloc();
        if (!legacy_frame_) throw std::runtime_error("RGBA frame allocation failed");
    }
}

RgbaFrameConverter::~RgbaFrameConverter() {
    if (context_) sws_freeContext(context_);
    av_frame_free(&legacy_frame_);
}

std::optional<app::VideoFrame> RgbaFrameConverter::Convert(const AVFrame *frame) {
    const auto begin = Clock::now();
    last_ = {};
    if (!frame || frame->width <= 0 || frame->height <= 0 || frame->width > 4096 || frame->height > 4096 ||
        (frame->format != AV_PIX_FMT_NV12 && frame->format != AV_PIX_FMT_YUV420P)) return {};
    context_ = sws_getCachedContext(context_, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
        frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!context_) throw std::runtime_error("RGBA conversion context failed");
    app::VideoFrame output;
    output.width = frame->width;
    output.height = frame->height;
    const auto row_bytes = static_cast<std::size_t>(frame->width) * 4;
    if (mode_ == RgbaConversionMode::LegacyCopy) {
        // Identical allocations, aligned intermediate and row copy to the stable client.
        output.buffer.resize(row_bytes * frame->height);
        last_.allocation = true;
        if (legacy_frame_->width != frame->width || legacy_frame_->height != frame->height) {
            av_frame_unref(legacy_frame_);
            legacy_frame_->format = AV_PIX_FMT_RGBA;
            legacy_frame_->width = frame->width;
            legacy_frame_->height = frame->height;
            if (av_frame_get_buffer(legacy_frame_, 32) < 0) {
                av_frame_unref(legacy_frame_);
                throw std::runtime_error("RGBA aligned legacy buffer allocation failed");
            }
        }
        if (av_frame_make_writable(legacy_frame_) < 0) throw std::runtime_error("RGBA legacy buffer is not writable");
        const auto convert_begin = Clock::now();
        const auto rows = sws_scale(context_, frame->data, frame->linesize, 0, frame->height,
                                    legacy_frame_->data, legacy_frame_->linesize);
        const auto convert_end = Clock::now();
        if (rows != frame->height) throw std::runtime_error("RGBA conversion returned incomplete image");
        for (int y = 0; y < frame->height; ++y)
            std::memcpy(output.buffer.data() + static_cast<std::size_t>(y) * row_bytes,
                        legacy_frame_->data[0] + static_cast<std::ptrdiff_t>(y) * legacy_frame_->linesize[0], row_bytes);
        const auto end = Clock::now();
        last_.convert_us = Micros(convert_begin, convert_end);
        last_.copy_us = Micros(convert_end, end);
        last_.copied_bytes = row_bytes * frame->height;
        last_.total_us = Micros(begin, end);
        return output;
    }
    std::shared_ptr<std::vector<std::uint8_t>> storage;
    for (auto &slot : pool_) {
        if (!slot) slot = std::make_shared<std::vector<std::uint8_t>>();
        if (slot.use_count() == 1) { storage = slot; break; }
    }
    if (!storage) { ++pool_busy_; return {}; }
    // SIMD conversion has an aligned base, aligned rows and 64 B of row padding.
    // Extra tail padding protects the last row as well. No pointer survives a resize.
    const std::size_t stride = (row_bytes + 64 + 63) & ~std::size_t(63);
    const std::size_t required = stride * frame->height + 127;
    if (storage->size() != required) { storage->resize(required); last_.allocation = true; }
    const auto address = reinterpret_cast<std::uintptr_t>(storage->data());
    const std::size_t offset = (64 - (address & 63)) & 63;
    output.shared_buffer = std::move(storage);
    output.pixel_offset = offset;
    output.row_pitch = stride;
    std::uint8_t *dest[] = {output.shared_buffer->data() + offset, nullptr, nullptr, nullptr};
    int pitches[] = {static_cast<int>(stride), 0, 0, 0};
    const auto convert_begin = Clock::now();
    const auto rows = sws_scale(context_, frame->data, frame->linesize, 0, frame->height, dest, pitches);
    const auto end = Clock::now();
    if (rows != frame->height) throw std::runtime_error("RGBA conversion returned incomplete image");
    last_.convert_us = Micros(convert_begin, end);
    last_.total_us = Micros(begin, end);
    return output;
}
} // namespace RoboGo::video
