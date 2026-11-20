#pragma once

#include "app/commander_measurement.h"
#include "app/application_types.h"
#include "arena/arena_view.h"
#include <spdlog/spdlog.h>

namespace RoboGo::app {

// Comparisons reuse one local session, its three clips and the same camera motion.
class CommanderBenchmark {
    using Clock = CommanderMeasurement::Clock;
    using Mode = arena::Mode;
    static constexpr std::array models{Mode::Flat, Mode::Light, Mode::Detailed};
    static constexpr auto warmup = std::chrono::seconds(5);
    static constexpr auto sample = std::chrono::seconds(60);
public:
    explicit CommanderBenchmark(CommanderComparison comparison) : comparison_(comparison) {}
    [[nodiscard]] bool Done() const { return stage_ == (Paired() ? 2 : 9); }
    [[nodiscard]] Mode Requested() const { return Paired() ? Mode::Light : models[stage_ % 3]; }
    [[nodiscard]] bool MarkersEnabled() const {
        return comparison_ == CommanderComparison::Layouts || (comparison_ == CommanderComparison::Markers && stage_ == 1);
    }
    [[nodiscard]] bool FloatingCards() const { return comparison_ == CommanderComparison::Layouts && stage_ == 1; }
    [[nodiscard]] Clock::time_point ReplayTime(Clock::time_point now) const {
        if (comparison_ != CommanderComparison::Layouts) return now;
        // Start both layout samples at the same place on the eight fictional tracks.
        return Clock::time_point(std::chrono::seconds(1)) + (measuring_ ? now - begin_ : Clock::duration::zero());
    }
    [[nodiscard]] double CameraSeconds() const {
        return measuring_ ? std::chrono::duration<double>(Clock::now() - begin_).count() : 0;
    }

    void Observe(bool fullscreen, bool presented, Mode actual, unsigned drawn,
                 unsigned fresh, int width, int height, std::size_t markers) {
        const auto now = Clock::now();
        const bool visible = actual == Requested() && drawn == 7 &&
                             (!Paired() || (markers == (MarkersEnabled() ? 8u : 0u) &&
                                                      width == 1920 && height == 1080));
        const char* log_group = "COMMANDER";
        const char* setting = "";
        if (comparison_ == CommanderComparison::Markers) {
            log_group = "COMMANDER_MARKERS";
            setting = MarkersEnabled() ? " markers=on" : " markers=off";
        } else if (comparison_ == CommanderComparison::Layouts) {
            log_group = "COMMANDER_LAYOUT";
            setting = FloatingCards() ? " layout=floating" : " layout=split";
        }
        if (!measuring_) {
            if (!fullscreen) { waiting_.reset(); ready_since_.reset(); return; }
            if (!waiting_) waiting_ = now;
            if (!visible || !presented) ready_since_.reset();
            else if (!ready_since_) ready_since_ = now;
            if (ready_since_ && now - *ready_since_ >= warmup) {
                begin_ = now;
                measuring_ = true;
                measurement_.Start(now, sample);
                SPDLOG_INFO("{}_BEGIN round={} mode={}{} width={} height={}", log_group, Round(), Name(), setting, width, height);
            } else if (now - *waiting_ > std::chrono::seconds(40)) {
                SPDLOG_WARN("{}_INVALID round={} mode={}{} reason=warmup_timeout drawn={} width={} height={} positions={}",
                            log_group, Round(), Name(), setting, drawn, width, height, markers);
                Next();
            }
            return;
        }
        if (!fullscreen || !visible) valid_ = false;
        if (!measurement_.Ended(now)) {
            if (presented) measurement_.Present(now, drawn & fresh);
            return;
        }
        const auto r = measurement_.Finish();
        SPDLOG_INFO("{}_RESULT round={} mode={}{} seconds=60 valid={} presents={} p95_ms={:.3f} longest_ms={:.3f} over33={} f1={} f3={} f4={} gap1_ms={:.3f} gap3_ms={:.3f} gap4_ms={:.3f}",
                    log_group, Round(), Name(), setting, valid_, r.presents, r.p95_interval_ms, r.longest_interval_ms, r.intervals_over_33ms,
                    r.lanes[0].frames, r.lanes[1].frames, r.lanes[2].frames,
                    r.lanes[0].longest_gap_ms, r.lanes[1].longest_gap_ms, r.lanes[2].longest_gap_ms);
        Next();
    }
private:
    bool Paired() const { return comparison_ != CommanderComparison::Arenas; }
    int Round() const { return stage_ / 3 + 1; }
    const char *Name() const {
        constexpr std::array names{"flat", "light", "detailed"};
        return Paired() ? "light" : names[stage_ % 3];
    }
    void Next() {
        ++stage_;
        measuring_ = false;
        valid_ = true;
        waiting_.reset();
        ready_since_.reset();
    }
    const CommanderComparison comparison_;
    int stage_{};
    bool measuring_{}, valid_{true};
    Clock::time_point begin_{};
    std::optional<Clock::time_point> waiting_, ready_since_;
    CommanderMeasurement measurement_;
};
} // namespace RoboGo::app
