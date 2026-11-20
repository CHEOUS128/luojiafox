#pragma once

#include "app/commander_measurement.h"
#include <spdlog/spdlog.h>

namespace RoboGo::app {

// Compare the two local operator views using the same two running replay lanes.
// Count a lane only if the new upload, its draw and Present all succeeded together.
// A failed swap invalidates the sample: it may also have discarded the pending upload.
class CupLayoutBenchmark {
    using Clock = CommanderMeasurement::Clock;
    static constexpr auto sample = std::chrono::seconds(30);
    static constexpr auto warmup = std::chrono::seconds(3);

public:
    bool Map() const { return stage_ == 1; }
    bool Done() const { return stage_ >= 2; }
    bool Passed() const { return Done() && valid_[0] && valid_[1] && meets_budget_; }

    void Observe(bool fullscreen, bool presented, int width, int height, unsigned drawn,
                 const std::array<bool, 3> &uploaded, bool correct_view, const std::array<int, 2> &ids) {
        const auto now = Clock::now();
        unsigned new_visible = 0;
        for (unsigned i = 0; i < 2; ++i) {
            if (uploaded[i] && (drawn & (1u << i))) new_visible |= 1u << i;
        }
        const bool expected = fullscreen && width == 1920 && height == 1080 && correct_view &&
                              ids == std::array<int, 2>{101, 104};
        const char *name = Map() ? "map" : "overview";
        if (!collecting_) {
            // Let the user press F11 at their own pace before timing starts.
            if (!fullscreen && !waiting_) return;
            if (!waiting_) waiting_ = now;
            if (expected && presented && drawn == 3u) {
                if (!ready_) ready_ = now;
            } else {
                ready_.reset();
            }
            if (ready_ && now - *ready_ >= warmup) {
                measurement_.Start(now, sample);
                collecting_ = true;
                SPDLOG_INFO("CUP_LAYOUT_BEGIN layout={} seat=103 peer_ids={}/{} size={}x{} seconds=30", name, ids[0], ids[1], width, height);
            } else if (now - *waiting_ >= std::chrono::seconds(40)) {
                SPDLOG_ERROR("CUP_LAYOUT_INVALID layout={} reason=warmup_timeout size={}x{} drawn={} view={} peer_ids={}/{}",
                             name, width, height, drawn, correct_view, ids[0], ids[1]);
                stage_ = 2;
            }
            return;
        }
        if (!measurement_.Ended(now)) {
            if (!expected || !presented) valid_[stage_] = false;
            if (presented) {
                if (drawn != 3u) ++missing_tiles_;
                measurement_.Present(now, new_visible);
            }
            return;
        }
        const auto result = measurement_.Finish();
        const auto &a = result.lanes[0];
        const auto &b = result.lanes[1];
        valid_[stage_] = valid_[stage_] && !missing_tiles_ && a.frames && b.frames;
        SPDLOG_INFO("CUP_LAYOUT_RESULT layout={} valid={} seconds=30 f1={} f4={} gap1_ms={:.3f} gap4_ms={:.3f} presents={} p95_ms={:.3f} missing_tiles={}",
                    name, valid_[stage_], a.frames, b.frames, a.longest_gap_ms, b.longest_gap_ms,
                    result.presents, result.p95_interval_ms, missing_tiles_);
        if (!Map()) {
            baseline_ = {a.frames, b.frames};
        } else if (baseline_[0] && baseline_[1]) {
            const double ratio1 = double(a.frames) / baseline_[0];
            const double ratio4 = double(b.frames) / baseline_[1];
            meets_budget_ = ratio1 >= .95 && ratio4 >= .95;
            SPDLOG_INFO("CUP_LAYOUT_COMPARE ratio1={:.5f} ratio4={:.5f} passed={}", ratio1, ratio4,
                        valid_[0] && valid_[1] && meets_budget_);
        }
        ++stage_;
        collecting_ = false;
        missing_tiles_ = 0;
        waiting_.reset();
        ready_.reset();
    }

private:
    CommanderMeasurement measurement_;
    std::array<unsigned, 2> baseline_{};
    std::array<bool, 2> valid_{true, true};
    std::optional<Clock::time_point> waiting_, ready_;
    int stage_{};
    unsigned missing_tiles_{};
    bool collecting_{}, meets_budget_{};
};

} // namespace RoboGo::app
