#pragma once

#include "main_window.h"

namespace RoboGo::app {
inline void ExpireHudTimers(State &state, std::chrono::steady_clock::time_point now) {
    if (state.match_notice && now >= state.match_notice->until) state.match_notice.reset();
    std::erase_if(state.active_buffs, [now](const BuffWithTimestamp &entry) {
        return now - entry.update_time >= std::chrono::seconds(entry.buff.buff_left_time());
    });
}

inline void ApplyMatchClock(State &state, GameStatus received) {
    if (received.has_current_round() && state.game_status && state.game_status->has_current_round() &&
        state.game_status->current_round() != received.current_round()) {
        if (state.competition == Competition::Ul2027) {
            // Keep the identity and log sequence of this login. Every other
            // displayed value belongs to the finished round until sent again.
            State next{};
            next.competition = state.competition;
            next.round_generation = state.round_generation + 1;
            next.next_log_sequence = state.next_log_sequence;
            if (state.robot_static_status && state.robot_static_status->has_robot_id()) {
                next.robot_static_status.emplace().set_robot_id(state.robot_static_status->robot_id());
            }
            state = std::move(next);
        } else if (state.competition == Competition::Uc2027) {
            state.global_unit_status.reset();
        }
    }
    state.game_status = std::move(received);
}

// 2027 protocol §2.2.4: own 1/3/4/7, then opposing 1/3/4/7.
// An incomplete array has no reliable boundary between teams.
inline std::optional<bool> BlueIdentity2027(const std::optional<RobotStaticStatus> &identity) {
    if (!identity || !identity->has_robot_id()) return {};
    const auto id = identity->robot_id();
    if (id >= 1 && id <= 9) return false;
    if (id >= 101 && id <= 109) return true;
    return {};
}

inline std::optional<uint32_t> Health2027(const std::optional<RobotStaticStatus> &identity,
                                          const std::optional<GlobalUnitStatus> &units,
                                          bool blue, int role) {
    const auto own_blue = BlueIdentity2027(identity);
    if (!own_blue || !units || units->robot_health_size() != 8) return {};
    constexpr std::array<int, 4> roles{1,3,4,7};
    auto slot = std::find(roles.begin(), roles.end(), role);
    if (slot == roles.end()) return {};
    const int side = *own_blue == blue ? 0 : 4;
    return units->robot_health(side + static_cast<int>(slot - roles.begin()));
}

inline constexpr uint32_t kUlStartingControlDegree = 600;

inline std::optional<uint32_t> ControlDegree2027(const State &state, bool blue) {
    const auto own_blue = BlueIdentity2027(state.robot_static_status);
    const auto &units = state.global_unit_status;
    if (!own_blue || !units) return {};
    if (*own_blue == blue) {
        if (units->has_total_cp_ally()) return units->total_cp_ally();
    } else if (units->has_total_cp_enemy()) {
        return units->total_cp_enemy();
    }
    return {};
}

// A reading above the *lower rule limit* proves which configuration is active.
// A damaged robot below that limit supplies no new evidence.
inline void ObserveUlHealthCaps(State &state) {
    if (state.competition != Competition::Ul2027) return;
    for (bool blue : {false, true}) {
        const auto heavy = Health2027(state.robot_static_status, state.global_unit_status, blue, 1);
        const auto infantry = Health2027(state.robot_static_status, state.global_unit_status, blue, 3);
        auto &caps = state.ul_roster_caps[blue];
        if (heavy && *heavy > 100 && *heavy <= 450) caps.heavy_at_450 = true;
        if (infantry && *infantry > 300 && *infantry <= 350) caps.infantry_at_350 = true;
    }
}

inline std::optional<uint32_t> HealthCap2027(const State &state, bool blue, int role) {
    const auto &identity = state.robot_static_status;
    if (identity && identity->has_robot_id() &&
        identity->robot_id() == static_cast<uint32_t>(role + (blue ? 100 : 0))) {
        if (identity->has_max_health() && identity->max_health() > 0) return identity->max_health();
        if (role == 3 && identity->has_performance_system_chassis()) {
            if (identity->performance_system_chassis() == 1) return 350;
            if (identity->performance_system_chassis() == 2) return 300;
        }
    }
    const auto &caps = state.ul_roster_caps[blue];
    if (role == 7) return 400;
    if (role == 1 && caps.heavy_at_450) return 450;
    if (role == 1 && state.game_status && state.game_status->has_current_stage() &&
        state.game_status->current_stage() == 3) return 100;
    if (role == 3 && caps.infantry_at_350) return 350;
    return {};
}
} // namespace RoboGo::app
