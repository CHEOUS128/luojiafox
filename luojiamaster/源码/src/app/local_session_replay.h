#pragma once

#include "app/local_session_capture.h"
#include "embedded_ul_capture.h"
#include <condition_variable>
#include <thread>

namespace RoboGo::app {

// Supplies the two recorded wire formats at their recorded times. The caller
// owns the very same receive queues used by the official UDP/MQTT callbacks.
class LocalSessionReplay {
public:
    using PacketSink = std::function<void(unsigned lane, local_capture::Bytes payload)>;
    using MatchTick = std::function<void(int)>;
    using Clock = std::chrono::steady_clock;
    LocalSessionReplay(PacketSink sink, std::function<void()> rewind)
        : sample_(local_capture::ReadSample({embedded_ul_capture_data, embedded_ul_capture_data_len})),
          sink_(std::move(sink)), rewind_(std::move(rewind)) {}

    void Start(Clock::time_point start, MatchTick match_tick) {
        if (worker_.joinable()) return;
        worker_ = std::jthread([this, start, tick = std::move(match_tick)](std::stop_token stop) {
            try {
                Play(stop, start, tick);
            } catch (const std::exception &error) {
                SPDLOG_ERROR("本地双路录流已停止 message={}", error.what());
            }
        });
    }

private:
    void Play(std::stop_token stop, Clock::time_point start, const MatchTick &match_tick) {
        auto epoch = start;
        int next_second = 0;
        bool rewind_before_packet = false;
        std::array<size_t, 2> next{};
        const auto &lanes = sample_.lanes;
        while (!stop.stop_requested()) {
            const bool official_done = next[0] == lanes[0].size();
            const bool camera_done = next[1] == lanes[1].size();
            if (official_done && camera_done) {
                epoch += sample_.period;
                next = {};
                rewind_before_packet = true;
            }
            const unsigned lane = next[1] == lanes[1].size() ||
                (next[0] < lanes[0].size() && lanes[0][next[0]].at <= lanes[1][next[1]].at) ? 0 : 1;
            const auto &packet = lanes[lane][next[lane]];
            const auto video_due = epoch + std::chrono::microseconds(packet.at);
            const auto tick_due = match_tick ? start + std::chrono::seconds(next_second) : Clock::time_point::max();
            const auto due = std::min(tick_due, video_due);
            {
                std::unique_lock lock(wait_mutex_);
                wake_.wait_until(lock, stop, due, [] { return false; });
            }
            if (stop.stop_requested()) return;
            const auto now = Clock::now();
            if (now - due > std::chrono::milliseconds(200)) {
                // After system sleep, start a fresh clip rather than release a stale burst.
                next = {};
                epoch = now;
                if (match_tick) {
                    const auto current = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(now - start).count());
                    if (current >= next_second) {
                        match_tick(current);
                        next_second = current + 1;
                    }
                }
                rewind_before_packet = false;
                rewind_();
                continue;
            }
            if (tick_due <= video_due) {
                match_tick(next_second++);
                continue;
            }
            if (rewind_before_packet) {
                rewind_();
                rewind_before_packet = false;
            }
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
            const bool interrupted = lane == 0 ? seconds >= 16 && seconds < 18
                                               : seconds >= 20 && seconds < 26;
            if (!interrupted) sink_(lane, packet.wire);
            ++next[lane];
        }
    }

    local_capture::Capture sample_;
    PacketSink sink_;
    std::function<void()> rewind_;
    std::mutex wait_mutex_;
    std::condition_variable_any wake_;
    // Last member: stop and join before callbacks or fixture metadata are destroyed.
    std::jthread worker_;
};

} // namespace RoboGo::app
