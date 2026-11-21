#include "app/match_values.h"
#include "app/cup_role_status.h"
#include "app/ul_live_view.h"
#include "app/trial_match.h"

#include <charconv>
#include <iostream>
#include <string_view>

using namespace RoboGo::app;

namespace {
int ReadPositive(std::string_view value) {
    int number = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || number <= 0) return 0;
    return number;
}

// Loopback QA feeds these exact bytes to the standard MQTT subscriber.
int EmitFixture(const char *id_arg, const char *duration_arg, std::string_view event) {
    if (event != "ul2027" && event != "uc2027") return 2;
    const int id = ReadPositive(id_arg);
    const int duration = ReadPositive(duration_arg);
    const bool allowed = id == 1 || id == 3 || id == 101 || id == 103 ||
                         (event == "uc2027" && (id == 4 || id == 104));
    if (!allowed || duration == 0 || duration > 3600) return 2;
    const Competition competition = event == "uc2027" ? Competition::Uc2027 : Competition::Ul2027;
    CupTrialSeat cup_seat;
    for (int second = 0; second < duration; ++second) {
        const auto emit = [&](const std::string &topic, const std::string &bytes) {
            static constexpr char hex[] = "0123456789abcdef";
            std::cout << second << ' ' << topic << ' ';
            for (unsigned char byte : bytes) std::cout << hex[byte >> 4] << hex[byte & 15];
            std::cout << '\n';
        };
        if (competition == Competition::Uc2027) cup_seat.Tick(id, second, emit);
        else EmitTrialMatch(competition, id, second, emit);
    }
    return std::cout ? 0 : 1;
}
}

int main(int argc, char **argv) {
    if ((argc == 4 || argc == 5) && std::string_view(argv[1]) == "--emit-fixture")
        return EmitFixture(argv[2], argv[3], argc == 5 ? argv[4] : "ul2027");
    if (argc != 1) return 2;
    int failures = 0;
    auto check = [&](bool result, const char *name) {
        if (!result) { ++failures; std::cerr << name << " failed\n"; }
    };
    {
        using namespace std::chrono;
        const auto prior = steady_clock::time_point{};
        const auto slow_tick = prior + milliseconds(580);
        const auto never = steady_clock::time_point::min();
        check(CanDrawVideoFrame(true, true, never, kUdpVideoFallbackTimeout, slow_tick),
              "first upload draws before the first successful Present");
        check(CanDrawVideoFrame(true, true, prior, kUdpVideoFallbackTimeout, slow_tick),
              "new UDP upload visible after a slow UI tick");
        check(!CanDrawVideoFrame(true, false, prior, kUdpVideoFallbackTimeout, slow_tick),
              "stopped UDP source waits after its timeout");
        check(!CanDrawVideoFrame(false, true, prior, kUdpVideoFallbackTimeout, slow_tick),
              "invalid texture cannot display");
        check(CanDrawVideoFrame(true, false, prior, kCustomVideoLiveTimeout, slow_tick) &&
              !CanDrawVideoFrame(true, false, prior, kCustomVideoLiveTimeout, prior + seconds(4)),
              "custom video retains its own timeout");
    }
    {
        UlHudView hud;
        check(!hud.HandleMapKeys(true, false, false) && !hud.map_open, "unsigned seat ignores M");
        hud.SelectIdentity(3);
        check(!hud.HandleMapKeys(true, false, true) && !hud.map_open, "typing M skips the map");
        check(!hud.HandleMapKeys(true, true, false) && !hud.map_open, "closed map leaves Escape to the menu");
        hud.menu_open = true;
        check(!hud.HandleMapKeys(true, false, false) && !hud.map_open, "menu owns M while open");
        hud.menu_open = false;
        hud.industrial_main = true;
        check(hud.HandleMapKeys(true, false, false) && hud.map_open, "M opens the map");
        hud.SelectIdentity(3);
        check(hud.HandleMapKeys(false, false, false) && hud.map_open, "map stays open for the same seat");
        check(hud.HandleMapKeys(true, false, false) && !hud.map_open, "second M closes the map");
        check(hud.industrial_main && !hud.menu_open, "closing the map restores the selected camera");
        hud.HandleMapKeys(true, false, false);
        check(hud.HandleMapKeys(false, true, false) && !hud.map_open && !hud.menu_open,
              "Escape closes only the map");
        check(!hud.HandleMapKeys(false, true, false), "next Escape is available for the normal menu");
        hud.HandleMapKeys(true, false, false);
        hud.SelectIdentity(101);
        check(!hud.map_open && hud.industrial_main, "new seat starts on its usual match view");
        hud.HandleMapKeys(true, false, false);
        hud.SelectIdentity(0);
        check(!hud.map_open && !hud.HandleMapKeys(true, false, false), "logout clears the map");
    }
    {
        for (const int seat : {1, 3, 4, 101, 103, 104})
            check(IsCupMapSeat(seat), "cup operator can open a map");
        for (const int seat : {-1, 0, 2, 7, 100, 102, 107, 203})
            check(!IsCupMapSeat(seat), "other identities keep their existing UI");
        UlHudView hud;
        hud.SelectIdentity(104);
        check(!hud.HandleMapKeys(true, false, false, true) && !hud.map_open,
              "cup menu and original marker mode retain the keyboard");
        check(hud.HandleMapKeys(true, false, false, false) && hud.map_open,
              "signed-in cup match opens its map");
        check(hud.HandleMapKeys(false, true, false, false) && !hud.map_open && !hud.menu_open,
              "cup Escape returns to match without also opening the menu");
    }
    const std::string request_topic = "RobotPerformanceSelectionCommand";
    RobotPerformanceSelectionCommand request;
    UlTrialLoadout loadout;
    loadout.Advance(3, 0);
    check(!loadout.Reply(request_topic, request.SerializeAsString()), "missing chassis gets no receipt");
    for (uint32_t preferred : {1u, 2u}) {
        request.set_chassis(preferred);
        request.set_shooter(1); // The legacy menu also sends unrelated fields.
        const auto response = loadout.Reply(request_topic, request.SerializeAsString());
        RobotPerformanceSelectionSync parsed;
        check(response && parsed.ParseFromString(*response) && parsed.has_chassis() &&
              parsed.chassis() == preferred && !parsed.has_shooter() && !parsed.has_sentry_control(),
              "local referee only echoes the chosen chassis");
    }
    check(!loadout.Reply("RobotDynamicStatus", request.SerializeAsString()), "wrong topic gets no receipt");
    check(!loadout.Reply(request_topic, "\xff"), "invalid request gets no receipt");
    request.set_chassis(3);
    check(!loadout.Reply(request_topic, request.SerializeAsString()), "unsupported preference gets no receipt");
    request.set_chassis(1);
    loadout.Reply(request_topic, request.SerializeAsString());
    bool selected_cap = false;
    EmitTrialMatch(Competition::Ul2027, 3, 1, [&](const auto &topic, const auto &wire) {
        if (topic != "RobotStaticStatus") return;
        RobotStaticStatus self;
        selected_cap = self.ParseFromString(wire) && self.max_health() == 350 && self.performance_system_chassis() == 1;
    }, loadout.chassis);
    check(selected_cap, "chosen health chassis supplied by local telemetry");
    loadout.Advance(3, 3);
    request.set_chassis(2);
    RobotPerformanceSelectionSync held;
    auto locked_reply = loadout.Reply(request_topic, request.SerializeAsString());
    check(locked_reply && held.ParseFromString(*locked_reply) && held.chassis() == 1, "in-play loadout remains locked");
    loadout.Advance(3, 39);
    check(loadout.chassis == 2, "new trial round restores power chassis");
    {
        State state{};
        state.competition = Competition::Ul2027;
        auto &source = state.global_unit_status.emplace();
        check(!ControlDegree2027(state, false), "unknown identity does not imply red");
        state.robot_static_status.emplace().set_robot_id(103);
        check(!ControlDegree2027(state, true), "legacy unit packet lacks control fields");
        check(source.ParseFromString(std::string("\x78\0", 2)), "official wire field 15");
        check(ControlDegree2027(state,true) == 0u && !ControlDegree2027(state,false), "blue confirmed zero, red absent");
        check(source.ParseFromString(std::string("\x80\x01\x07", 3)), "official wire field 16");
        check(!ControlDegree2027(state,true) && ControlDegree2027(state,false) == 7u, "blue absent, red present");
        for (int hp : {90,150,0,240,80,150,0,190}) source.add_robot_health(hp);
        ObserveUlHealthCaps(state);
        check(!HealthCap2027(state,false,3) && !HealthCap2027(state,true,1), "midgame damaged robot has unknown cap");
        check(HealthCap2027(state,true,7) == 400u, "sentry rule cap");
        source.set_robot_health(4, 140);
        source.set_robot_health(5, 310);
        ObserveUlHealthCaps(state);
        source.set_robot_health(4, 50);
        source.set_robot_health(5, 150);
        ObserveUlHealthCaps(state);
        check(HealthCap2027(state,false,1) == 450u && HealthCap2027(state,false,3) == 350u
              && !HealthCap2027(state,true,3), "configuration proved for correct team across later damage");
        state.robot_static_status->set_max_health(300);
        check(HealthCap2027(state,true,3) == 300u, "reported capacity belongs to self");
        state.robot_static_status->clear_max_health();
        state.robot_static_status->set_performance_system_chassis(1);
        check(HealthCap2027(state,true,3) == 350u, "confirmed local chassis maps via rule");
    }
    for (const int id : {1, 3, 101, 103}) {
        State state{};
        state.competition = Competition::Ul2027;
        int notice_count = 0;
        auto run = [&](int second) {
            const auto now = std::chrono::steady_clock::time_point{} + std::chrono::seconds(second);
            EmitTrialMatch(state.competition, id, second, [&](const std::string &topic, const std::string &bytes) {
                if (topic == "GameStatus") {
                    GameStatus arrived;
                    check(arrived.ParseFromString(bytes), "clock wire data");
                    ApplyMatchClock(state, std::move(arrived));
                } else if (topic == "RobotStaticStatus") {
                    check(state.robot_static_status.emplace().ParseFromString(bytes), "identity wire data");
                    ObserveUlHealthCaps(state);
                } else if (topic == "GlobalUnitStatus") {
                    check(state.global_unit_status.emplace().ParseFromString(bytes), "roster wire data");
                    ObserveUlHealthCaps(state);
                } else if (topic == "RobotModuleStatus") {
                    check(state.robot_module_status.emplace().ParseFromString(bytes), "device wire data");
                } else if (topic == "RobotDynamicStatus") {
                    check(state.robot_dynamic_status.emplace().ParseFromString(bytes), "body wire data");
                } else if (topic == "Event") {
                    Event event;
                    check(event.ParseFromString(bytes) && event.event_id() == 1 && event.param() == "103,3",
                          "elimination matches roster");
                    ++notice_count;
                    state.match_notice = TimedMatchNotice{"event", now + std::chrono::seconds(5)};
                } else if (topic == "Buff") {
                    Buff buff;
                    const int end = 9;
                    check(buff.ParseFromString(bytes) && buff.robot_id() == static_cast<uint32_t>(id) &&
                          buff.buff_type() == 2 && buff.buff_left_time() == static_cast<uint32_t>(end - second),
                          "buff belongs to current seat and expires on time");
                    state.active_buffs.assign(1, BuffWithTimestamp{std::move(buff), now});
                } else check(false, "unexpected topic in trial fixture");
            });
            ExpireHudTimers(state, now);
        };
        const auto hp = [&](bool blue, int role) {
            return Health2027(state.robot_static_status, state.global_unit_status, blue, role);
        };
        run(0);
        check(ControlDegree2027(state,false) == 600u && ControlDegree2027(state,true) == 600u, "opening control 600/600");
        check(HealthCap2027(state,false,1) == 100u && HealthCap2027(state,true,1) == 100u, "opening heavies 100");
        check(!state.robot_dynamic_status && !state.robot_module_status, "no premature device or body report");
        run(3);
        check(hp(false,1) == 100u && hp(true,1) == 100u && hp(false,3) == 300u && hp(true,3) == 350u
              && hp(false,7) == 400u && hp(true,7) == 400u, "six rule-correct opening HP values after either login");
        check(hp(false,4) == 0u && hp(true,4) == 0u, "empty slot 4 not used as sentry");
        check(!Health2027({},state.global_unit_status,true,3), "unknown identity");
        check(!state.robot_dynamic_status->has_current_chassis_energy(), "reserved field not used");
        check(state.robot_dynamic_status->current_buffer_energy() == 60 &&
              state.robot_static_status->max_buffer_energy() == 100, "buffer energy source");
        check(!state.robot_module_status->has_laser_detection_module(), "2026 laser field not used");
        run(4);
        check(state.active_buffs.size() == 1, "buff appeared");
        run(7);
        check(hp(false,3) == 150u && hp(true,3) == 150u && notice_count == 0, "damaged infantry before elimination");
        if (id == 3) check(HealthCap2027(state,false,3) == 300u, "power chassis half health is 150/300");
        check(state.robot_dynamic_status->current_health() == *hp(id >= 100, id % 100), "local and roster HP agree");
        run(9);
        check(state.active_buffs.empty() && !state.match_notice, "buff expired");
        check(ControlDegree2027(state,true) == 599u && ControlDegree2027(state,false) == 600u, "zone costs opposing side one point");
        check(state.game_status->is_paused() && state.game_status->stage_countdown_sec() == 19, "pause");
        const auto energy_at_pause = state.robot_dynamic_status->current_buffer_energy();
        run(13);
        check(ControlDegree2027(state,true) == 599u, "pause freezes control degree");
        check(state.game_status->is_paused() && state.game_status->stage_countdown_sec() == 19 &&
              state.robot_dynamic_status->current_buffer_energy() == energy_at_pause, "pause freeze");
        run(15);
        check(!state.game_status->is_paused() && state.game_status->stage_countdown_sec() == 18, "resume");
        check(state.robot_module_status->has_rfid() && state.robot_module_status->rfid() == 0, "RFID offline");
        run(18);
        check(ControlDegree2027(state,true) == 595u, "zone losses track running seconds");
        check(state.robot_module_status->rfid() == 1, "RFID returned");
        run(19);
        check(ControlDegree2027(state,true) == 585u && hp(true,3) == 0u && notice_count == 1 &&
              state.match_notice.has_value(), "elimination and ten-point loss on the same tick");
        check(state.robot_dynamic_status->current_health() == *hp(id >= 100,id % 100), "elimination matches own body HP");
        state.global_unit_status->mutable_robot_health()->RemoveLast();
        check(!hp(true,3), "incomplete roster");
        run(33);
        check(state.game_status->game_result() == 1 && state.game_status->current_stage() == 5 &&
              state.game_status->end_reason() == 2, "red wins by control degree");
        state.chassis_return = ReturnedPreference{2, std::chrono::steady_clock::now()};
        state.system_logs.emplace_back();
        state.vehicle_logs.emplace_back();
        // A packet just before the new round must not survive the boundary,
        // even when its normal timeout has not yet elapsed.
        state.match_notice = TimedMatchNotice{"last notice", std::chrono::steady_clock::time_point::max()};
        Buff pending;
        pending.set_buff_left_time(120);
        state.active_buffs.push_back({pending, std::chrono::steady_clock::time_point{}});
        GameStatus next_round;
        next_round.set_current_round(2);
        ApplyMatchClock(state,std::move(next_round));
        check(!ControlDegree2027(state,false) && !hp(true,3) && !HealthCap2027(state,true,3), "round clears score and configuration evidence");
        run(39);
        check(state.game_status->current_round() == 2 && ControlDegree2027(state,true) == 600u && hp(true,3) == 350u && !state.robot_dynamic_status &&
              !state.robot_module_status && state.active_buffs.empty() && state.system_logs.empty() &&
              state.vehicle_logs.empty(), "fresh round");
        check(!state.match_notice && !state.chassis_return, "no old notice or preference in next round");
        state.competition = Competition::Uc2026;
        run(42);
        check(state.global_unit_status && state.global_unit_status->robot_health_size() == 10, "legacy roster");
        check(!state.robot_dynamic_status && !state.robot_module_status, "legacy fixture preserved");
    }
    {
        State state{};
        state.competition = Competition::Uc2027;
        auto &units = state.global_unit_status.emplace();
        // Official wire fields 1=ally base 0, 3=control state 2, 4=R site 0,
        // 6=opposing base 120, 8=control state 1. B site intentionally absent.
        check(units.ParseFromString(std::string("\x08\x00\x18\x02\x20\x00\x30\x78\x40\x01", 10)), "UC wire fields");
        check(!CupBaseHp(state, false) && !CupBaseHp(state, true), "unknown seat cannot assign base colors");
        check(CupOutpostHp(state, false) == 0u && !CupOutpostHp(state, true), "R site zero and absent B independent of seat");
        auto &identity = state.robot_static_status.emplace();
        for (int seat : {1, 103}) {
            identity.set_robot_id(seat);
            check(CupBaseHp(state, seat >= 100) == 0u && CupBaseHp(state, seat < 100) == 120u,
                  "UC relative base HP ignores legacy shield values");
            check(CupOutpostHp(state, false) == 0u && !CupOutpostHp(state, true), "fixed sites do not swap on blue login");
        }
        check(units.ParseFromString(std::string("\x48\x00", 2)) && !CupOutpostHp(state, false) &&
              CupOutpostHp(state, true) == 0u && !CupBaseHp(state, true), "field 9 zero survives while fields 1/4 are absent");
    }
    check(CupAssemblyResultCode("0") == 0 && CupAssemblyResultCode("8") == 8 &&
          CupAssemblyResultCode("9") == 9, "UC assembly result preserves zero and unknown codes");
    check(!CupAssemblyResultCode("") && !CupAssemblyResultCode("2,0") && !CupAssemblyResultCode("256"),
          "UC malformed assembly result has no value");
    for (const int seat : {1, 3, 4, 101, 103, 104}) {
        State state{};
        state.competition = Competition::Uc2027;
        std::vector<std::pair<uint32_t, std::string>> events;
        auto sample = [&](int second) {
            EmitTrialMatch(state.competition, seat, second, [&](const std::string &topic, const std::string &wire) {
                if (topic == "GameStatus") {
                    GameStatus clock;
                    check(clock.ParseFromString(wire), "UC clock payload");
                    ApplyMatchClock(state, std::move(clock));
                } else if (topic == "RobotStaticStatus") {
                    check(state.robot_static_status.emplace().ParseFromString(wire), "UC identity payload");
                } else if (topic == "GlobalUnitStatus") {
                    check(state.global_unit_status.emplace().ParseFromString(wire), "UC scoreboard payload");
                } else if (topic == "GlobalLogisticsStatus") {
                    check(state.global_logistics_status.emplace().ParseFromString(wire), "UC logistics payload");
                } else if (topic == "Event") {
                    Event notice;
                    check(notice.ParseFromString(wire), "UC notice payload");
                    events.emplace_back(notice.event_id(), notice.param());
                    if (notice.event_id() == 15) state.cup_last_assembly_result = CupAssemblyResultCode(notice.param());
                }
            });
        };
        sample(0);
        check(!CupBaseHp(state, false) && !CupOutpostHp(state, true), "UC waits for unit packet");
        check(state.robot_static_status && !state.robot_static_status->has_performance_system_chassis() &&
              !state.global_logistics_status && !state.cup_last_assembly_result, "UC seat waits for first role data");
        sample(3);
        const int role = seat % 100;
        check(state.global_logistics_status && state.global_logistics_status->has_tech_level() &&
              state.global_logistics_status->tech_level() == 0, "UC reported starting tech level is zero");
        if (role == 1) check(!state.robot_static_status->has_performance_system_chassis(), "UC heavy has no infantry chassis");
        else check(state.robot_static_status->has_performance_system_chassis() &&
                   state.robot_static_status->performance_system_chassis() == (role == 3 ? 2u : 1u),
                   "UC confirmed chassis matches the selected infantry seat");
        check(CupBaseHp(state, true) == 10000u && CupOutpostHp(state, true) == 1000u &&
              ControlDegree2027(state, false) == 0u && ControlDegree2027(state, true) == 0u, "UC starting reserves and zero points");
        sample(6);
        sample(8);
        check(state.cup_last_assembly_result == 2, "UC new public assembly report: timeout");
        sample(9);
        check(state.game_status && state.game_status->is_paused() && state.game_status->stage_countdown_sec() == 19, "UC clock paused");
        const auto paused_red = ControlDegree2027(state, false);
        sample(13);
        check(state.game_status && state.game_status->stage_countdown_sec() == 19 && paused_red == ControlDegree2027(state, false),
              "UC clock and points stay still during pause");
        sample(16);
        check(state.game_status && !state.game_status->is_paused() && state.game_status->stage_countdown_sec() < 19, "UC clock resumed");
        check(CupBaseHp(state, false) == 9800u && CupBaseHp(state, true) == 9250u &&
              CupOutpostHp(state, false) == 870u && CupOutpostHp(state, true) == 640u,
              "UC red/blue bases and fixed R/B outposts agree across six seats");
        sample(19);
        check(state.cup_last_assembly_result == 0 && state.global_logistics_status &&
              state.global_logistics_status->has_tech_level() &&
              state.global_logistics_status->tech_level() == (seat < 100 ? 1u : 0u),
              "UC public assembly success and independent team-specific tech level");
        sample(23);
        sample(28);
        const std::vector<std::pair<uint32_t, std::string>> expected_events{
            {5,"1"}, {15,"2"}, {6,"2"}, {15,"0"}, {7,"2"}, {8,"1"}};
        check(events == expected_events, "UC local notices use 2027 ids and absolute parameters");
        sample(33);
        const auto red = ControlDegree2027(state, false), blue = ControlDegree2027(state, true);
        check(state.game_status && state.game_status->current_stage() == 5 && state.game_status->game_result() == 1 &&
              state.game_status->end_reason() == 2 && red && blue && *red > *blue, "UC settlement reports red ahead by cumulative control");
        state.match_notice = TimedMatchNotice{"old", std::chrono::steady_clock::time_point::max()};
        state.system_logs.emplace_back();
        sample(39);
        check(state.game_status && state.game_status->current_round() == 2 && state.round_generation == 1 &&
              !state.match_notice && state.system_logs.empty() && !state.global_unit_status &&
              state.robot_static_status && state.robot_static_status->robot_id() == static_cast<uint32_t>(seat),
              "UC new round clears prior board and notices but preserves the seat");
        check(!state.global_logistics_status && !state.cup_last_assembly_result &&
              !state.robot_static_status->has_performance_system_chassis(), "UC new round clears prior role data");
    }
    {
        for (const int seat : {3, 4, 103, 104}) {
            CupTrialSeat ref;
            RobotStaticStatus self;
            RobotPerformanceSelectionSync receipt;
            GameStatus stage;
            int receipts = 0;
            const auto emit = [&](const std::string &topic, const std::string &bytes) {
                if (topic == "RobotStaticStatus") check(self.ParseFromString(bytes), "cup menu identity");
                if (topic == "GameStatus") check(stage.ParseFromString(bytes), "cup menu stage");
                if (topic == "RobotPerformanceSelectionSync") {
                    ++receipts;
                    check(receipt.ParseFromString(bytes), "cup menu receipt");
                }
            };
            RobotPerformanceSelectionCommand selection;
            selection.set_chassis(2);
            auto send = [&] { return ref.Receive(request_topic, selection.SerializeAsString()); };
            check(!send(), "unsigned seat cannot submit");
            ref.Tick(seat, 0, emit);
            check(stage.current_stage() == 1 && !self.has_performance_system_chassis(),
                  "cup preparation starts awaiting hardware profile");
            selection.set_chassis(0);
            check(!send(), "zero chassis is invalid");
            selection.set_chassis(2);
            check(send() && receipts == 0, "cup submitted but still awaiting independent receipt");
            ref.Tick(seat, 1, emit);
            check(receipts == 1 && receipt.has_chassis() && receipt.chassis() == 2 &&
                  !receipt.has_shooter() && !receipt.has_sentry_control() &&
                  self.robot_id() == static_cast<uint32_t>(seat) && self.performance_system_chassis() == 2,
                  "cup four infantry seats receive only the requested chassis field");
            ref.Tick(seat, 22, emit);
            check(stage.current_stage() == 2 && send(), "self-check still allows selection");
            for (int second : {25, 28, 34, 39, 58}) {
                ref.Tick(seat, second, emit);
                check(!send(), "countdown, active, paused and settled phases lock selection");
                check(self.performance_system_chassis() == 2, "chosen profile survives throughout its round");
            }
            ref.Tick(seat, kCupTrialPeriod, emit);
            check(stage.current_round() == 2 && stage.current_stage() == 1 &&
                  !self.has_performance_system_chassis(), "next round clears previous selection");
            ref.Tick(seat, kCupTrialPeriod + 28, emit);
            check(self.performance_system_chassis() == 1, "cup default health chassis when no choice made");
            ref.Tick(seat, kCupTrialPeriod * 2, emit);
            check(send(), "queued selection before seat change");
            const int previous_receipts = receipts;
            ref.Tick(1, kCupTrialPeriod * 2 + 1, emit);
            check(!send() && !self.has_performance_system_chassis() && receipts == previous_receipts,
                  "heavy seat cannot submit or inherit previous infantry request");
        }
        CupTrialSeat ref;
        TechCoreMotionStateSync core;
        RobotRespawnStatus revival;
        const auto emit = [&](const std::string &topic, const std::string &bytes) {
            if (topic == "TechCoreMotionStateSync") check(core.ParseFromString(bytes), "cup local prop state");
            if (topic == "RobotRespawnStatus") check(revival.ParseFromString(bytes), "cup local revival state");
        };
        AssemblyCommand operation;
        auto send = [&] { return ref.Receive("AssemblyCommand", operation.SerializeAsString()); };
        for (int seat : {1, 101}) {
            ref.Tick(seat, 0, emit);
            operation.set_operation(0);
            operation.set_difficulty(1);
            check(!send(), "cup assembly waits for an active match");
            ref.Tick(seat, 28, emit);
            operation.set_difficulty(2);
            check(core.basic_state() == 1 && core.maximum_difficulty_level() == 1 && !send(),
                  "cup mock prop only offers its published level");
            operation.set_difficulty(1);
            check(send() && !send(), "cup assembly begins once");
            ref.Tick(seat, 29, emit);
            check(core.basic_state() == 2 && core.putin_state() == 0, "cup local prop reports motion");
            operation.clear_difficulty();
            operation.set_operation(1);
            check(!send(), "cannot confirm while prop moving");
            ref.Tick(seat, 31, emit);
            check(core.basic_state() == 3 && core.putin_state() == 1 && send(),
                  "cup reached and inserted prop can be confirmed");
            ref.Tick(seat, 32, emit);
            check(core.basic_state() == 1 && !send(), "cup confirmation returns prop to idle");
            operation.set_operation(0);
            operation.set_difficulty(1);
            check(send(), "cup assembly starts again");
            ref.Tick(seat, 33, emit);
            ref.Tick(seat, 34, emit);
            operation.set_operation(2);
            check(core.basic_state() == 2 && !send(), "cup pause prevents cancellation");
            ref.Tick(seat, 39, emit);
            check(core.basic_state() == 2 && send(), "prop clock held during pause, can now cancel");
            ref.Tick(seat, 40, emit);
            check(core.basic_state() == 1 && !send(), "cancel resets activity on next tick");
        }
        ref.Tick(103, 32, emit);
        check(revival.has_is_pending_respawn() && revival.is_pending_respawn() &&
              !ref.Receive("AssemblyCommand", operation.SerializeAsString()), "different seat has independent status");
    }
    if (!failures) std::cout << "UL and legacy fixtures passed. UC 2027: six seats, scoreboard, chassis, tech levels, assembly, menu phases, referee sync and round reset passed\n";
    return failures ? 1 : 0;
}
