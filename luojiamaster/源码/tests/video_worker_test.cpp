#include "video/custom_byte_block_video_decoder.h"
#include "video/custom_byte_block_video_decoder_worker.h"
#include "platform/platform_util.h"

#include <cstdio>

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

namespace {
using namespace RoboGo::video;
using Clock = std::chrono::steady_clock;

int Run(const std::string &fixture) {
    try {
        std::atomic<unsigned> initialization_errors{0};
        CustomByteBlockVideoDecoderConfig invalid;
        invalid.max_assembly_bytes = 0;
        {
            CustomByteBlockVideoDecoderWorker worker({}, [&](int type, const std::string &) {
                if (type == 2) initialization_errors.fetch_add(1);
            }, invalid);
            const auto deadline = Clock::now() + std::chrono::seconds(2);
            while (initialization_errors.load() == 0 && Clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            worker.stop();
        }
        CHECK(initialization_errors.load() != 0);

        // Actual FFmpeg parser receives non-Annex-B chunks, across the assembly
        // limit, then an explicit stream reset. This must return, not grow forever.
        CustomByteBlockVideoDecoderConfig bounded;
        bounded.max_assembly_bytes = 1024;
        CustomByteBlockVideoDecoder decoder(bounded);
        std::array<std::uint8_t, 300> garbage;
        garbage.fill(0xFF);
        garbage[0] = 1; garbage[1] = 42;
        for (unsigned sequence = 0; sequence < 12; ++sequence) {
            garbage[2] = static_cast<std::uint8_t>(sequence >> 8);
            garbage[3] = static_cast<std::uint8_t>(sequence);
            decoder.push_packet_bytes(garbage);
        }
        decoder.reset_stream();

        if (fixture.empty()) {
            std::puts("PASS video worker: constructor failure logs safely; bounded malformed parser input; empty stop");
            return 0;
        }
        auto file = RoboGo::platform_util::OpenBinaryInput(fixture);
        CHECK(file.is_open());
        std::vector<std::uint8_t> packets((std::istreambuf_iterator<char>(file)), {});
        CHECK(!packets.empty() && packets.size() % 300 == 0 && packets.size() <= 32U * 1024U * 1024U);
        unsigned decoded = 0;
        for (std::size_t offset = 0; offset < packets.size(); offset += 300) {
            auto frames = decoder.push_packet_bytes(std::span(packets.data() + offset, 300));
            for (const auto &frame : frames) {
                CHECK(frame.width > 0 && frame.height > 0);
                CHECK(frame.rgba.size() == static_cast<std::size_t>(frame.width) * frame.height * 4);
                ++decoded;
            }
        }
        CHECK(decoded != 0); // recovery after malformed input; no flush needed

        std::atomic<bool> first_frame{false};
        CustomByteBlockVideoDecoderWorker worker([&](CustomByteBlockDecodedFrame &&frame) {
            CHECK(!frame.rgba.empty());
            if (!first_frame.exchange(true)) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }, {});
        for (std::size_t offset = 0; offset < packets.size() && !first_frame.load(); offset += 300) {
            worker.enqueue_packet_bytes(std::span(packets.data() + offset, 300));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const auto frame_deadline = Clock::now() + std::chrono::seconds(2);
        while (!first_frame.load() && Clock::now() < frame_deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(first_frame.load());
        for (unsigned index = 0; index < 2048; ++index)
            worker.enqueue_packet_bytes(std::span(packets.data(), 300));
        CHECK(worker.queued_packets() <= 128);
        CHECK(worker.packets_dropped() > 0);
        const auto stop_start = Clock::now();
        worker.stop();
        const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - stop_start).count();
        CHECK(stop_ms < 1000);
        CHECK(worker.queued_packets() == 0);
        std::printf("PASS video worker: recovered=%u frames; pending<=128; overflow=%llu; backlog-stop=%lldms; no shutdown flush\n",
                    decoded, static_cast<unsigned long long>(worker.packets_dropped()), static_cast<long long>(stop_ms));
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL video worker: %s\n", error.what());
        return 1;
    }
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    return Run(argc > 1 ? RoboGo::platform_util::PathToUtf8(std::filesystem::path(argv[1])) : std::string{});
}
#else
int main(int argc, char **argv) { return Run(argc > 1 ? argv[1] : std::string{}); }
#endif
