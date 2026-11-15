#include "video/video_decoder.h"
#include "video/rgba_frame_converter.h"
#include "platform/platform_util.h"
#include <cstdio>

namespace {
using namespace RoboGo;
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)

std::vector<std::uint8_t> ReadVideo(const std::string &path) {
    auto input = platform_util::OpenBinaryInput(path);
    CHECK(input.is_open());
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    CHECK(!bytes.empty() && bytes.size() <= 512U * 1024U * 1024U);
    return bytes;
}

std::uint64_t Decode(const std::vector<std::uint8_t> &bytes, const std::function<void(const AVFrame *, std::uint64_t)> &callback) {
    std::uint64_t count = 0;
    video::VideoDecoder decoder([&](const AVFrame *frame) { callback(frame, ++count); });
    constexpr std::size_t chunk_size = 32768;
    std::vector<std::uint8_t> chunk;
    for (std::size_t offset = 0; offset < bytes.size(); offset += chunk_size) {
        const auto size = std::min(chunk_size, bytes.size() - offset);
        chunk.assign(bytes.begin() + offset, bytes.begin() + offset + size);
        decoder.decode_frame(chunk);
    }
    CHECK(count > 30); // Every timing pass must have a real warm-up interval.
    return count;
}

void CheckPixels(const app::VideoFrame &a, const app::VideoFrame &b) {
    CHECK(a.width == b.width && a.height == b.height && a.width > 0 && a.height > 0);
    CHECK(a.Pixels() && b.Pixels());
    const auto row_bytes = static_cast<std::size_t>(a.width) * 4;
    CHECK(a.RowPitch() >= row_bytes && b.RowPitch() >= row_bytes);
    CHECK(a.ByteSize() >= static_cast<std::size_t>(a.height - 1) * a.RowPitch() + row_bytes);
    CHECK(b.ByteSize() >= static_cast<std::size_t>(b.height - 1) * b.RowPitch() + row_bytes);
    for (int y = 0; y < a.height; ++y)
        CHECK(std::memcmp(a.Pixels() + static_cast<std::size_t>(y) * a.RowPitch(),
                          b.Pixels() + static_cast<std::size_t>(y) * b.RowPitch(), row_bytes) == 0);
}

int Main(const std::vector<std::string> &args) {
    try {
        CHECK(args.size() == 3);
        spdlog::set_level(spdlog::level::warn);
        const auto bytes = ReadVideo(args[1]);
        std::uint64_t compared_bytes = 0;
        video::RgbaFrameConverter legacy_pixels(video::RgbaConversionMode::LegacyCopy);
        video::RgbaFrameConverter optimized_pixels(video::RgbaConversionMode::ReuseAligned);
        // Pixel comparison is a separate decode pass. No hash/memcmp work enters
        // any timing sample or warms one mode's pixels before the other is timed.
        const auto pixel_frames = Decode(bytes, [&](const AVFrame *source, std::uint64_t) {
            auto a = legacy_pixels.Convert(source), b = optimized_pixels.Convert(source);
            CHECK(a && b); CheckPixels(*a, *b);
            compared_bytes += static_cast<std::uint64_t>(source->width) * source->height * 4;
        });
        std::printf("PASS real conversion pixels: frames=%llu rgba_bytes=%llu row_pixels_identical=1\n",
                    static_cast<unsigned long long>(pixel_frames), static_cast<unsigned long long>(compared_bytes));
        auto csv = platform_util::OpenUtf8Output(args[2]);
        CHECK(csv.is_open());
        csv << "round,decoded_index,width,height,source_format,mode,total_us,convert_us,copy_us,copied_bytes,allocation,warmup\n";
        std::uint64_t measured_samples = 0;
        for (unsigned round = 0; round < 3; ++round) {
            video::RgbaFrameConverter legacy(video::RgbaConversionMode::LegacyCopy);
            video::RgbaFrameConverter optimized(video::RgbaConversionMode::ReuseAligned);
            const auto frames = Decode(bytes, [&](const AVFrame *source, std::uint64_t index) {
                // Alternate order both per decoded image and per full-source pass.
                // Each mode receives the SAME decoder output at native resolution.
                for (unsigned order = 0; order < 2; ++order) {
                    const bool candidate = ((index + round + order) & 1) != 0;
                    auto &converter = candidate ? optimized : legacy;
                    auto result = converter.Convert(source);
                    CHECK(result.has_value());
                    const auto sample = converter.LastSample();
                    const bool warmup = index <= 30;
                    csv << round << ',' << index << ',' << source->width << ',' << source->height << ',' << source->format << ','
                        << (candidate ? "reuse_aligned" : "legacy_copy") << ',' << sample.total_us << ',' << sample.convert_us << ','
                        << sample.copy_us << ',' << sample.copied_bytes << ',' << sample.allocation << ',' << warmup << '\n';
                    if (!warmup) ++measured_samples;
                }
            });
            CHECK(frames == pixel_frames);
        }
        CHECK(csv.good() && measured_samples > 0);
        std::printf("PASS real conversion timing: rounds=3 warmup_each=30 measured_samples=%llu same_decoded_image_pairs=1 timing_includes_hash=0 scope=CPU_conversion_component_not_online_latency\n",
                    static_cast<unsigned long long>(measured_samples));
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL real conversion benchmark: %s\n", error.what()); return 1;
    }
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(RoboGo::platform_util::PathToUtf8(std::filesystem::path(argv[i])));
    return Main(args);
}
#else
int main(int argc, char **argv) { return Main(std::vector<std::string>(argv, argv + argc)); }
#endif
