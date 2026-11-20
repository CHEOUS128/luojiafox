#pragma once

#include "arena/replay_marker.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <optional>
#include <vector>

namespace RoboGo::app {

// Fixture for the UC local spectator seat. Its only input is elapsed session time.
class CommanderMarkerReplay {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;

    void Start(Time now) {
        start_ = now;
        blue3_paused_at_.reset();
        visible_.clear();
        visible_.reserve(routes_.size());
    }

    bool Blue3Paused() const { return blue3_paused_at_.has_value(); }

    void ToggleBlue3(Time now) {
        if (Blue3Paused()) blue3_paused_at_.reset();
        else blue3_paused_at_ = now;
    }

    const std::vector<arena::ReplayMarker>& Visible(Time now) {
        visible_.clear();
        if (!start_ || now < *start_) return visible_;
        for (const auto& route : routes_) {
            const Time through = route.team == 1 && route.number == 3 && Blue3Paused()
                               ? std::min(now, *blue3_paused_at_) : now;
            // Each 250 ms sample has a session-relative timestamp. A late UI
            // frame selects the latest available sample instead of queuing missed ones.
            const auto tick = (through - *start_) / sample_interval_;
            if (tick < 1) continue;
            const auto sample_time = *start_ + tick * sample_interval_;
            const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - sample_time).count();
            if (age >= 10) continue;
            const auto phase = static_cast<int>((tick - 1) % (2 * route.leg_ticks));
            const float progress = static_cast<float>(std::min(phase, 2 * route.leg_ticks - phase))
                                 / static_cast<float>(route.leg_ticks);
            visible_.push_back({route.team, route.number,
                                route.u0 + (route.u1 - route.u0) * progress,
                                route.v0 + (route.v1 - route.v0) * progress,
                                static_cast<int>(age), route.FloorAt(progress)});
        }
        return visible_;
    }

private:
    struct Route {
        int team, number;
        float u0, v0, u1, v1;
        int leg_ticks;
        // Local mesh heights at both ends and the middle. Red 3 crosses
        // the gentle center ridge of the sample arena.
        float floor_start, floor_middle, floor_end;
        float FloorAt(float t) const {
            return t < .5f ? std::lerp(floor_start, floor_middle, 2*t)
                           : std::lerp(floor_middle, floor_end, 2*t-1);
        }
    };
    static constexpr auto sample_interval_ = std::chrono::milliseconds(250);
    // Eight independent fictional movements in normalized map coordinates.
    static constexpr std::array<Route, 8> routes_{{
        {0, 1, .14f, .24f, .27f, .24f, 40,  .0970f,  .0970f,  .0970f},
        {0, 3, .23f, .58f, .32f, .42f, 24, -.0322f, -.0010f, -.0326f},
        {0, 4, .08f, .75f, .18f, .65f, 32, -.0996f, -.0796f, -.0597f},
        {0, 7, .26f, .64f, .33f, .68f, 48, -.0558f, -.0636f, -.0719f},
        {1, 1, .86f, .72f, .77f, .70f, 36, -.0876f, -.0839f, -.0797f},
        {1, 3, .78f, .40f, .69f, .35f, 28, -.0400f, -.0499f, -.0600f},
        {1, 4, .87f, .38f, .82f, .32f, 44, -.0481f, -.0593f, -.0716f},
        {1, 7, .67f, .13f, .62f, .12f, 52,  .0539f,  .0520f,  .0499f},
    }};
    std::optional<Time> start_;
    std::optional<Time> blue3_paused_at_;
    std::vector<arena::ReplayMarker> visible_;
};
} // namespace RoboGo::app
