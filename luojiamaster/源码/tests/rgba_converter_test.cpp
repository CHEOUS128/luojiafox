#include "video/rgba_frame_converter.h"
#include <iostream>

namespace {
using RoboGo::video::RgbaFrameConverter;
using RoboGo::video::RgbaConversionMode;
void Check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
using Frame = std::unique_ptr<AVFrame, void(*)(AVFrame*)>;
Frame Make(int width, int height, AVPixelFormat format, int seed) {
    Frame frame(av_frame_alloc(), [](AVFrame *p) { av_frame_free(&p); });
    Check(bool(frame), "source allocation");
    frame->width = width; frame->height = height; frame->format = format;
    Check(av_frame_get_buffer(frame.get(), 64) >= 0, "source buffer allocation");
    const int planes = format == AV_PIX_FMT_NV12 ? 2 : 3;
    for (int plane = 0; plane < planes; ++plane) {
        const int rows = plane == 0 ? height : (height + 1) / 2;
        const int columns = plane == 0 ? width : (format == AV_PIX_FMT_NV12 ? ((width + 1) / 2) * 2 : (width + 1) / 2);
        for (int y = 0; y < rows; ++y) for (int x = 0; x < columns; ++x)
            frame->data[plane][y * frame->linesize[plane] + x] = static_cast<uint8_t>((x * 37 + y * 19 + seed * 17 + plane * 71) & 255);
    }
    return frame;
}
void Equal(const RoboGo::app::VideoFrame &a, const RoboGo::app::VideoFrame &b) {
    Check(a.width == b.width && a.height == b.height, "different dimensions");
    Check(a.Pixels() && b.Pixels(), "missing pixels");
    for (int y = 0; y < a.height; ++y)
        Check(std::memcmp(a.Pixels() + y * a.RowPitch(), b.Pixels() + y * b.RowPitch(), a.width * 4) == 0, "different pixels");
}
}

int main() {
    try {
        for (auto format : {AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P}) {
            RgbaFrameConverter old(RgbaConversionMode::LegacyCopy), optimized;
            for (const auto &[width, height] : {std::pair{128, 128}, {129, 127}, {320, 256}, {1920, 1080}, {127, 129}}) {
                auto source = Make(width, height, format, width);
                auto a = old.Convert(source.get()), b = optimized.Convert(source.get());
                Check(a.has_value() && b.has_value(), "missing converted frame");
                Equal(*a, *b);
                Check((reinterpret_cast<uintptr_t>(b->Pixels()) & 63) == 0 && b->RowPitch() % 64 == 0, "unaligned pixels");
            }
        }
        RgbaFrameConverter converter;
        auto first = Make(1920, 1080, AV_PIX_FMT_NV12, 1);
        auto a = converter.Convert(first.get());
        Check(a.has_value(), "first frame");
        std::vector<uint8_t> saved(a->Pixels(), a->Pixels() + a->width * 4);
        auto other = Make(129, 127, AV_PIX_FMT_YUV420P, 7);
        auto b = converter.Convert(other.get());
        auto c = converter.Convert(other.get());
        Check(b.has_value() && c.has_value(), "retained frames");
        Check(!converter.Convert(other.get()).has_value() && converter.PoolBusyCount() == 1, "pool grew past bound");
        Check(std::memcmp(a->Pixels(), saved.data(), saved.size()) == 0, "reused an owned buffer");
        b.reset();
        Check(converter.Convert(other.get()).has_value(), "pool slot was not recycled");
        std::cout << "PASS RGBA converter: NV12/YUV420P; landscape/portrait/odd sizes; byte-identical; 64B alignment; bounded ownership and resize protection\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
