#pragma once

#include "app/application_types.h"
#include "app/cup_role_status.h"

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

// Both operator and commander see the same clock. The local excerpt starts
// with three seconds of countdown and pauses with 19 seconds remaining.
inline GameStatus TrialClock(Competition competition, int elapsed_seconds) {
    GameStatus clock;
    if (elapsed_seconds < 0) return clock;
    const int tick = elapsed_seconds % kTrialRoundSeconds;
    const bool paused = tick >= 9 && tick < 14;
    const bool settled = tick >= 33;
    clock.set_current_stage(tick < 3 ? 3 : settled ? 5 : 4);
    clock.set_is_paused(paused);
    const int seconds_left = tick < 3 ? 3 - tick : tick < 9 ? 28 - tick :
                             paused ? 19 : std::max(0, 33 - tick);
    clock.set_stage_countdown_sec(seconds_left);
    if (competition == Competition::Uc2027) {
        const int round = elapsed_seconds / kTrialRoundSeconds % 2 + 1;
        clock.set_current_round(round);
        clock.set_total_rounds(3);
        clock.set_red_score(round - 1 + (settled ? 1 : 0));
        clock.set_blue_score(0);
        clock.set_game_result(settled ? 1 : 255);
        clock.set_end_reason(settled ? 2 : 255);
    } else {
        const bool ul = competition == Competition::Ul2027;
        clock.set_current_round(elapsed_seconds / kTrialRoundSeconds + 1);
        clock.set_game_result(settled ? (ul ? 1 : 0) : 255);
        if (ul && settled) clock.set_end_reason(2);
    }
    return clock;
}

// Each interactive UC round has time to open the menu before the 39-second
// match excerpt. The operator and commander share this clock.
inline constexpr int kCupTrialPreparationSeconds = 25;
inline constexpr int kCupTrialPeriod = kCupTrialPreparationSeconds + kTrialRoundSeconds;

inline int CupTrialExcerptSecond(int second) {
    return second / kCupTrialPeriod * kTrialRoundSeconds +
           std::max(0, second % kCupTrialPeriod - kCupTrialPreparationSeconds);
}

inline GameStatus CupTrialClock(int second) {
    if (second < 0) return {};
    GameStatus clock = TrialClock(Competition::Uc2027, CupTrialExcerptSecond(second));
    const int phase_second = second % kCupTrialPeriod;
    if (phase_second < kCupTrialPreparationSeconds) {
        constexpr int self_check_start = kCupTrialPreparationSeconds - 3;
        const bool preparing = phase_second < self_check_start;
        clock.set_current_stage(preparing ? 1 : 2);
        clock.set_stage_countdown_sec((preparing ? self_check_start : kCupTrialPreparationSeconds) - phase_second);
    }
    return clock;
}

inline void FillCupTrialScore(GlobalUnitStatus &units, bool blue, int tick) {
    const int red_base = tick < 7 ? 10000 : 9800;
    const int blue_base = tick < 7 ? 10000 : 9250;
    units.set_base_health(blue ? blue_base : red_base);
    units.set_enemy_base_health(blue ? red_base : blue_base);
    // Field 4 is always site R and field 9 is always site B in 2027.
    units.set_outpost_health(tick < 7 ? 1000 : 870);
    units.set_enemy_outpost_health(tick < 16 ? 1000 : 640);
    const int running = std::clamp(tick - 3, 0, 6) + std::clamp(tick - 14, 0, 19);
    const int red_points = running * 2;
    const int blue_points = running;
    units.set_total_cp_ally(blue ? blue_points : red_points);
    units.set_total_cp_enemy(blue ? red_points : blue_points);
}

inline void EmitCupTrialEvent(int tick, const TrialMessageSink &emit) {
    Event notice;
    switch (tick) {
        case 6:  notice.set_event_id(5); notice.set_param("1"); break;
        case 8:  notice.set_event_id(15); notice.set_param("2"); break;
        case 16: notice.set_event_id(6); notice.set_param("2"); break;
        case 19: notice.set_event_id(15); notice.set_param("0"); break;
        case 23: notice.set_event_id(7); notice.set_param("2"); break;
        case 28: notice.set_event_id(8); notice.set_param("1"); break;
        default: return;
    }
    emit("Event", notice.SerializeAsString());
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
                           const TrialMessageSink &emit, int chassis = 0,
                           const GameStatus *session_clock = nullptr) {
    if (elapsed_seconds < 0 || robot_id <= 0) return;
    const int tick = elapsed_seconds % kTrialRoundSeconds;
    const bool ul = competition == Competition::Ul2027;
    const bool cup = competition == Competition::Uc2027;
    if (ul && chassis == 0) chassis = DefaultUlChassis(robot_id);
    const GameStatus clock = session_clock ? *session_clock : TrialClock(competition, elapsed_seconds);
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
    } else if (cup && (tick >= 3 || chassis != 0)) {
        const int role = robot_id % 100;
        if (tick >= 3) {
            identity.set_alive_state(TrialHealth(robot_id, tick) ? 1 : 2);
            if (role == 3 || role == 4) identity.set_level(1);
        }
        if (role == 3 || role == 4) {
            // The old recorded example has a preset for each seat. The
            // interactive mock referee supplies its own actual selection.
            identity.set_performance_system_chassis(chassis ? chassis : (role == 3 ? 2 : 1));
        }
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
    } else if (cup) {
        FillCupTrialScore(units, robot_id >= 100, tick);
    }
    emit("GlobalUnitStatus", units.SerializeAsString());
    if (ul && tick >= 3) EmitUlHudSample(robot_id, tick, chassis, emit);
    if (cup) {
        GlobalLogisticsStatus logistics;
        // Only the separate logistics message says which team's level has changed.
        logistics.set_tech_level(robot_id < 100 && tick >= 19 ? 1 : 0);
        logistics.set_remaining_economy(1000);
        emit("GlobalLogisticsStatus", logistics.SerializeAsString());
        RobotDynamicStatus body;
        body.set_current_health(TrialHealth(robot_id, tick));
        body.set_can_remote_heal(body.current_health() > 0 && tick >= 16 && tick < 33);
        emit("RobotDynamicStatus", body.SerializeAsString());
        RobotRespawnStatus revival;
        const bool waiting = body.current_health() == 0;
        revival.set_is_pending_respawn(waiting);
        revival.set_can_free_respawn(false);  // This last-minute excerpt ends before Blue 3 earns a free return.
        revival.set_can_pay_for_respawn(waiting);
        if (waiting) revival.set_gold_cost_for_respawn(580); // Last minute, level 1, table 5-7.
        emit("RobotRespawnStatus", revival.SerializeAsString());
        EmitCupTrialEvent(tick, emit);
    }
}

class CupTrialSeat {
public:
    bool Receive(const std::string &topic, const std::string &payload) {
        if (topic == "AssemblyCommand") return QueueAssembly(payload);
        if (topic != "RobotPerformanceSelectionCommand" || !can_change_) return false;
        RobotPerformanceSelectionCommand request;
        if (!request.ParseFromString(payload) || !request.has_chassis() ||
            (request.chassis() != 1 && request.chassis() != 2)) return false;
        pending_chassis_ = static_cast<int>(request.chassis());
        return true;
    }

    void Tick(int seat, int second, const TrialMessageSink &emit) {
        if (second < 0 || seat <= 0) {
            *this = {};
            return;
        }
        const int round = second / kCupTrialPeriod;
        if (seat_ != seat || round_ != round) {
            *this = {};
            seat_ = seat;
            round_ = round;
        }
        const GameStatus clock = CupTrialClock(second);
        const auto stage = clock.current_stage();
        const bool infantry = seat == 3 || seat == 4 || seat == 103 || seat == 104;
        can_change_ = infantry && !clock.is_paused() && CupChassisStage(stage);
        if (pending_chassis_) chassis_ = *pending_chassis_;
        if (infantry && !chassis_ && stage >= 4) chassis_ = 1;
        EmitTrialMatch(Competition::Uc2027, seat, CupTrialExcerptSecond(second), emit, chassis_, &clock);
        const bool heavy = seat == 1 || seat == 101;
        assembly_open_ = heavy && stage == 4 && !clock.is_paused();
        if (heavy && stage == 4) {
            if (pending_assembly_) {
                if (*pending_assembly_ == 0) {
                    basic_state_ = 2;
                    started_with_remaining_ = clock.stage_countdown_sec();
                } else {
                    basic_state_ = 1;
                }
                pending_assembly_.reset();
            }
            if (basic_state_ == 2 && started_with_remaining_ - clock.stage_countdown_sec() >= 2) basic_state_ = 3;
            TechCoreMotionStateSync core;
            core.set_maximum_difficulty_level(1);
            core.set_basic_state(basic_state_);
            core.set_putin_state(basic_state_ == 3 ? 1 : 0);
            core.set_move_state(0);
            core.set_rotate_state(0);
            emit("TechCoreMotionStateSync", core.SerializeAsString());
        }
        if (pending_chassis_) {
            RobotPerformanceSelectionSync sync;
            sync.set_chassis(*pending_chassis_);
            emit("RobotPerformanceSelectionSync", sync.SerializeAsString());
            pending_chassis_.reset();
        }
    }

private:
    bool QueueAssembly(const std::string &payload) {
        if (!assembly_open_ || pending_assembly_) return false;
        AssemblyCommand request;
        if (!request.ParseFromString(payload) || !request.has_operation()) return false;
        const auto action = request.operation();
        const bool start = action == 0 && basic_state_ == 1 &&
                           request.has_difficulty() && request.difficulty() == 1;
        const bool confirm = action == 1 && basic_state_ == 3;
        const bool cancel = action == 2 && basic_state_ != 1;
        if (!start && !confirm && !cancel) return false;
        pending_assembly_ = action;
        return true;
    }

    int seat_ = 0;
    int round_ = -1;
    int chassis_ = 0;
    bool can_change_ = false;
    std::optional<int> pending_chassis_;
    bool assembly_open_ = false;
    uint32_t basic_state_ = 1;
    int started_with_remaining_ = 0;
    std::optional<uint32_t> pending_assembly_;
};
} // namespace RoboGo::app
