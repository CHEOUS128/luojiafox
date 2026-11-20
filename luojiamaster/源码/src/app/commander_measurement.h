#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace RoboGo::app {

// One half-open wall-clock window. Call Present only after a successful swap.
// Long freezes at either edge count towards the longest video stall.
class CommanderMeasurement {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;

    struct Lane {
        unsigned frames{};
        double longest_gap_ms{};
    };
    struct Result {
        std::array<Lane, 3> lanes{};
        unsigned presents{};
        unsigned intervals_over_33ms{};
        double p95_interval_ms{};
        double longest_interval_ms{};
    };

    void Start(Time now, Clock::duration duration) {
        start_ = now;
        end_ = now + duration;
        last_present_ = now;
        last_new_.fill(now);
        intervals_.clear();
        result_ = {};
    }

    bool Ended(Time now) const { return now >= end_; }

    void Present(Time now, unsigned new_visible) {
        if (now < start_ || now >= end_) return;
        if (now > last_present_) RecordInterval(now - last_present_);
        last_present_ = now;
        ++result_.presents;
        for (unsigned i = 0; i < last_new_.size(); ++i) {
            if ((new_visible & (1u << i)) == 0) continue;
            auto &lane = result_.lanes[i];
            lane.longest_gap_ms = std::max(lane.longest_gap_ms, Milliseconds(now - last_new_[i]));
            ++lane.frames;
            last_new_[i] = now;
        }
    }

    Result Finish() {
        if (end_ > last_present_) RecordInterval(end_ - last_present_);
        for (unsigned i = 0; i < last_new_.size(); ++i) {
            auto &gap = result_.lanes[i].longest_gap_ms;
            gap = std::max(gap, Milliseconds(end_ - last_new_[i]));
        }
        if (!intervals_.empty()) {
            const auto rank = static_cast<size_t>(std::ceil(intervals_.size() * 0.95)) - 1;
            std::nth_element(intervals_.begin(), intervals_.begin() + rank, intervals_.end());
            result_.p95_interval_ms = intervals_[rank];
        }
        return result_;
    }
private:
    static double Milliseconds(Clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
    }
    void RecordInterval(Clock::duration d) {
        const double ms = Milliseconds(d);
        intervals_.push_back(ms);
        if (ms > 33.) ++result_.intervals_over_33ms;
        result_.longest_interval_ms = std::max(result_.longest_interval_ms, ms);
    }
    Time start_{}, end_{}, last_present_{};
    std::array<Time, 3> last_new_{};
    Result result_{};
    std::vector<double> intervals_;
};

} // namespace RoboGo::app
