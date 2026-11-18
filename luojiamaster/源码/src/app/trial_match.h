#pragma once

#include "app/application_types.h"

namespace RoboGo::app {

using TrialMessageSink = std::function<void(const std::string &, const std::string &)>;

inline constexpr int kTrialRoundSeconds = 39;

inline int DefaultUlChassis(int id) { return id == 103 ? 1 : 2; }

// Local referee records the chosen infantry chassis during the countdown.
// Later clicks receive the already locked setting, as in the 3V3 rules.
struct UlTrialLoadout {
    int robot = 0, round = -1, chassis = 0;
    bool locked = true;

    void Advance(int id, int second) {
        if (id != robot || second / kTrialRoundSeconds != round) {
            robot = id;
            round = second / kTrialRoundSeconds;
            chassis = DefaultUlChassis(id);
        }
        locked = second % kTrialRoundSeconds >= 3;
    }

    std::optional<std::string> Reply(const std::string &topic, const std::string &payload) {
        if ((robot != 3 && robot != 103) || topic != "RobotPerformanceSelectionCommand") return {};
        RobotPerformanceSelectionCommand request;
        if (!request.ParseFromString(payload) || !request.has_chassis() ||
            (request.chassis() != 1 && request.chassis() != 2)) return {};
        if (!locked) chassis = static_cast<int>(request.chassis());
        RobotPerformanceSelectionSync ack;
        ack.set_chassis(chassis);
        return ack.SerializeAsString();
    }
};

inline int TrialHealth(int id, int tick) {
    if (id == 103 && tick >= 7) return 0;
    return (id < 100 ? 100 : 200) + id % 100;
}

inline int UlTrialMaxHealth(int id, int seat, int chassis) {
    switch (id % 100) {
        case 1: return 100;  // No module installation in this excerpt.
        case 3: return (id == seat ? chassis : DefaultUlChassis(id)) == 1 ? 350 : 300;
        case 7: return 400;
        default: return 0;   // Protocol slot 4 is unused in 3V3.
    }
}

inline int UlTrialHealth(int id, int tick, int seat, int chassis) {
    if (id == 103 && tick >= 19) return 0;
    if (id % 100 == 3 && tick >= 6) return 150;
    return UlTrialMaxHealth(id, seat, chassis);
}

inline int UlTrialBlueControl(int tick) {
    // Red takes one zone at 8s. The clock pauses at 9s and resumes at 14s.
    // Five running seconds in control cost blue five points; the zone then lapses.
    const int zone_loss = (tick >= 9 ? 1 : 0) + std::clamp(tick - 14, 0, 4);
    return 600 - zone_loss - (tick >= 19 ? 10 : 0);
}

// Passive fixtures for the UL desk. The 2027 protocol reserves dynamic field 4
// and reuses module field 11. Neither legacy field is supplied here.
inline void EmitUlHudSample(int id, int tick, int chassis, const TrialMessageSink &emit) {
    const int active_seconds = std::max(0, std::min(tick, 9) - 3)
                             + std::clamp(tick - 14, 0, 19);
    RobotDynamicStatus body;
    body.set_current_health(UlTrialHealth(id, tick, id, chassis));
    body.set_current_buffer_energy(60 + active_seconds);
    body.set_current_experience(20 + active_seconds);
    body.set_experience_for_upgrade(80 - active_seconds);
    body.set_current_heat(0);
    body.set_remaining_ammo(100);
    emit("RobotDynamicStatus", body.SerializeAsString());

    RobotModuleStatus modules;
    modules.set_power_manager(1);
    modules.set_rfid(tick >= 14 && tick < 18 ? 0 : 1);
    modules.set_light_strip(1);
    modules.set_uwb(1);
    modules.set_armor(1);
    modules.set_video_transmission(1);
    modules.set_capacitor(1);
    modules.set_main_controller(1);
    emit("RobotModuleStatus", modules.SerializeAsString());

    if (tick == 19) {
        Event notice;
        notice.set_event_id(1);
        notice.set_param("103,3");
        emit("Event", notice.SerializeAsString());
    }
    const int buff_end = 9;
    if (tick >= 4 && tick < buff_end) {
        Buff buff;
        buff.set_robot_id(id);
        buff.set_buff_type(2);
        buff.set_buff_level(10);
        buff.set_buff_max_time(buff_end - 4);
        buff.set_buff_left_time(buff_end - tick);
        emit("Buff", buff.SerializeAsString());
    }
}

// A short, repeatable end-of-round excerpt. Video uses its own recorded timestamps.
inline void EmitTrialMatch(Competition competition, int robot_id, int elapsed_seconds,
                           const TrialMessageSink &emit, int chassis = 0) {
    if (elapsed_seconds < 0 || robot_id <= 0) return;
    const int tick = elapsed_seconds % kTrialRoundSeconds;
    const int round = elapsed_seconds / kTrialRoundSeconds + 1;
    const bool ul = competition == Competition::Ul2027;
    if (chassis == 0) chassis = DefaultUlChassis(robot_id);
    GameStatus clock;
    clock.set_current_round(round);
    const bool paused = tick >= 9 && tick < 14;
    const bool settled = tick >= 33;
    clock.set_current_stage(tick < 3 ? 3 : settled ? 5 : 4);
    clock.set_is_paused(paused);
    const int seconds_left = tick < 3 ? 3 - tick : (tick < 9 ? 28 - tick :
                             (paused ? 19 : std::max(0, 33 - tick)));
    clock.set_stage_countdown_sec(seconds_left);
    clock.set_game_result(settled ? (ul ? 1 : 0) : 255);
    if (ul && settled) clock.set_end_reason(2); // Control degree.
    emit("GameStatus", clock.SerializeAsString());

    RobotStaticStatus identity;
    identity.set_robot_id(robot_id);
    if (ul) {
        identity.set_connection_state(1);
        identity.set_field_state(0);
        identity.set_alive_state(UlTrialHealth(robot_id, tick, robot_id, chassis) == 0 ? 2 : 1);
        identity.set_level(1);
        identity.set_max_health(UlTrialMaxHealth(robot_id, robot_id, chassis));
        if (robot_id % 100 == 3) identity.set_performance_system_chassis(chassis);
        identity.set_max_heat(100);
        identity.set_max_buffer_energy(100);
    }
    emit("RobotStaticStatus", identity.SerializeAsString());
    if (!ul && tick < 3) return;

    GlobalUnitStatus units;
    constexpr std::array<int,5> old_roles{1,2,3,4,7};
    constexpr std::array<int,4> new_roles{1,3,4,7};
    const auto roles = competition == Competition::Uc2026 ? std::span<const int>(old_roles)
                                                          : std::span<const int>(new_roles);
    for (int team = 0; team < 2; ++team) {
        const bool blue = (robot_id >= 100) != (team != 0);
        for (auto role : roles) {
            const int id = (blue ? 100 : 0) + role;
            units.add_robot_health(ul ? UlTrialHealth(id, tick, robot_id, chassis) : TrialHealth(id, tick));
        }
    }
    if (ul) {
        units.set_total_cp_ally(robot_id >= 100 ? UlTrialBlueControl(tick) : 600);
        units.set_total_cp_enemy(robot_id >= 100 ? 600 : UlTrialBlueControl(tick));
    }
    emit("GlobalUnitStatus", units.SerializeAsString());
    if (ul && tick >= 3) EmitUlHudSample(robot_id, tick, chassis, emit);
}
} // namespace RoboGo::app
