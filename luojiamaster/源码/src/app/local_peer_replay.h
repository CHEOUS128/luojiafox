#pragma once

#include "embedded_peer_view_one.h"
#include "embedded_peer_view_two.h"
#include "embedded_peer_view_three.h"
#include "platform/platform_util.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace RoboGo::app {

// Each file contains HEVC access units with a four-byte big-endian length.
// Every loop starts with parameter sets and a keyframe.
class LocalPeerReplay {
    using Bytes = std::span<const std::uint8_t>;
    using Frames = std::vector<Bytes>;
    using Sink = std::function<void(std::size_t, std::vector<std::uint8_t> &&)>;
    using Clock = std::chrono::steady_clock;

    struct Lane {
        std::vector<std::uint8_t> file_bytes;
        Frames frames;
        std::mutex mutex;
        std::condition_variable_any wake;
        bool paused = false;
        unsigned revision = 0;
        // This must join before releasing the mutex, sample spans or file bytes.
        std::jthread worker;
    };

    static Frames ReadFrames(Bytes bytes) {
        Frames frames;
        while (!bytes.empty()) {
            if (bytes.size() < 4) throw std::runtime_error("Incomplete local peer sample header");
            const auto size = (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16)
                            | (std::uint32_t(bytes[2]) << 8) | bytes[3];
            bytes = bytes.subspan(4);
            if (!size || size > bytes.size() || size > 8 * 1024 * 1024 || frames.size() >= 4096)
                throw std::runtime_error("Invalid local peer frame length or count");
            frames.push_back(bytes.first(size));
            bytes = bytes.subspan(size);
        }
        if (frames.empty()) throw std::runtime_error("Empty local peer sample");
        return frames;
    }

    static std::vector<std::uint8_t> ReadFile(const std::filesystem::path &path) {
        const auto name = platform_util::PathToUtf8(path);
        auto input = platform_util::OpenBinaryInput(name);
        if (!input) throw std::runtime_error("Cannot open commander replay: " + name);
        input.seekg(0, std::ios::end);
        const auto length = static_cast<std::streamoff>(input.tellg());
        if (length <= 0 || length > 64 * 1024 * 1024)
            throw std::runtime_error("Invalid commander replay file size: " + name);
        input.seekg(0);
        std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
        if (!input.read(reinterpret_cast<char *>(data.data()), length))
            throw std::runtime_error("Incomplete commander replay file: " + name);
        return data;
    }

    static void Play(Lane &lane, std::size_t slot, const Sink &sink,
                     Clock::duration interval, std::stop_token stop) {
        std::unique_lock lock(lane.mutex);
        std::size_t next = 0;
        unsigned revision = lane.revision;
        auto due = Clock::now();
        while (!stop.stop_requested()) {
            if (lane.paused) {
                lane.wake.wait(lock, stop, [&] { return !lane.paused; });
                continue;
            }
            if (revision != lane.revision || Clock::now() - due > 3 * interval) {
                due = Clock::now();
                next = 0;
                revision = lane.revision;
            }
            const auto frame = lane.frames[next];
            // The lock only covers this lane and a bounded enqueue. Pausing
            // synchronizes with the last submit before rebuilding its decoder.
            sink(slot, {frame.begin(), frame.end()});
            next = (next + 1) % lane.frames.size();
            due += interval;
            lane.wake.wait_until(lock, stop, due, [&] { return revision != lane.revision; });
        }
    }

public:
    LocalPeerReplay(std::span<const int> seat_ids, Sink sink, const std::string &video_dir = {})
        : count_(seat_ids.size()) {
        if (count_ > lanes_.size()) throw std::invalid_argument("Maximum of three peer views");
        const std::array<Bytes, 3> embedded{{
            {embedded_peer_view_one_data, embedded_peer_view_one_data_len},
            {embedded_peer_view_two_data, embedded_peer_view_two_data_len},
            {embedded_peer_view_three_data, embedded_peer_view_three_data_len}
        }};
        for (std::size_t slot = 0; slot < count_; ++slot) {
            auto &lane = lanes_[slot];
            // Clip 1 is the front camera. Clips 2 and 3 are distinct industrial-camera recordings.
            unsigned clip;
            switch (seat_ids[slot]) {
                case 1: case 101: clip = 1; break;
                case 3: case 103: clip = 2; break;
                case 4: case 104: clip = 3; break;
                default: throw std::invalid_argument("No recorded clip for this seat");
            }
            if (video_dir.empty()) {
                lane.frames = ReadFrames(embedded[clip - 1]);
            } else {
                auto path = platform_util::PathFromUtf8(video_dir) / ("peer-view-" + std::to_string(clip) + ".bin");
                lane.file_bytes = ReadFile(path);
                lane.frames = ReadFrames(lane.file_bytes);
            }
        }
        const auto interval = std::chrono::nanoseconds(1'000'000'000 / (video_dir.empty() ? 15 : 30));
        for (std::size_t slot = 0; slot < count_; ++slot) {
            auto &lane = lanes_[slot];
            lane.worker = std::jthread([&lane, slot, sink, interval](std::stop_token stop) {
                Play(lane, slot, sink, interval, stop);
            });
        }
    }

    void SetPaused(std::size_t slot, bool paused) {
        if (slot >= count_) return;
        auto &lane = lanes_[slot];
        {
            const std::lock_guard lock(lane.mutex);
            lane.paused = paused;
            ++lane.revision;
        }
        lane.wake.notify_all();
    }

private:
    std::size_t count_;
    std::array<Lane, 3> lanes_;
};

} // namespace RoboGo::app
