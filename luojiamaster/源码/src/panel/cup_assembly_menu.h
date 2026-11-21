#pragma once

#include "app/cup_role_status.h"
#include "main_window.h"
#include <chrono>
#include <cstdint>
#include <string>

struct CupAssemblyMenuState {
    std::uint32_t selected_level = 1;
    std::string last_request;
    std::chrono::steady_clock::time_point last_sent{};
    std::chrono::steady_clock::time_point last_confirm{};
    std::uint64_t generation = 0;
    std::uint32_t seat = 0;
};

// RMUC 2027 protocol §§2.2.21–22: these are requests to the referee server.
// A public result (Event 15) carries no seat, team or request identifier.
inline void ShowCupAssemblyMenu(const State &state, RoboGo::io::MqttClient &client,
                                bool in_play, CupAssemblyMenuState &menu) {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    const auto now = Clock::now();
    const auto seat = state.robot_static_status ? state.robot_static_status->robot_id() : 0;
    if (menu.generation != state.round_generation || menu.seat != seat) {
        menu = {};
        menu.generation = state.round_generation;
        menu.seat = seat;
    }
    if (state.competition != RoboGo::app::Competition::Uc2027 || (seat != 1 && seat != 101)) return;

    ImGui::TextUnformatted("科技核心 · 能量单元装配");
    const auto &core = state.tech_core_motion_state_sync;
    const auto known = core && core->has_basic_state() && core->basic_state() >= 1 && core->basic_state() <= 3;
    const auto fresh = core && state.cup_core_received_at != Clock::time_point{} &&
                       now - state.cup_core_received_at <= 3s;
    const char *pose = !core || !core->has_basic_state() ? "等待" :
                       core->basic_state() == 1 ? "初始位置" : core->basic_state() == 2 ? "运动中" :
                       core->basic_state() == 3 ? "已到位" : "未知";
    const auto max_level = core && core->has_maximum_difficulty_level() ? core->maximum_difficulty_level() : 0;
    const bool levels_known = max_level >= 1 && max_level <= 4;
    ImGui::Text("核心位置：%s", pose);
    ImGui::SameLine();
    if (levels_known) ImGui::Text("最高难度：%u 级", static_cast<unsigned>(max_level));
    else if (core && core->has_maximum_difficulty_level() && max_level == 0) ImGui::TextUnformatted("最高难度：0 级");
    else ImGui::TextUnformatted("最高难度：等待有效数据");
    const auto flag = [](bool has, std::uint32_t value) {
        return !has ? "等待" : value == 0 ? "未完成" : value == 1 ? "已完成" : "未知";
    };
    if (core) {
        ImGui::Text("放入 %s   平移 %s   旋转 %s",
                    flag(core->has_putin_state(), core->putin_state()),
                    flag(core->has_move_state(), core->move_state()),
                    flag(core->has_rotate_state(), core->rotate_state()));
    } else ImGui::TextUnformatted("放入 等待   平移 等待   旋转 等待");
    const auto bulletin = state.cup_last_assembly_result
        ? RoboGo::app::CupAssemblyResultName(*state.cup_last_assembly_result) : "等待";
    ImGui::Text("全场最近通报：%s", bulletin.c_str());

    const bool playing = in_play && state.game_status && state.game_status->has_current_stage() &&
                         state.game_status->current_stage() == 4 && state.game_status->has_is_paused() &&
                         !state.game_status->is_paused();
    const bool enabled = playing && known && fresh &&
                         client.get_connection_state() == RoboGo::io::MqttClient::ConnectionState::Connected;
    const bool alive = state.robot_static_status->has_alive_state() && state.robot_static_status->alive_state() == 1;
    const bool can_start = enabled && alive && levels_known && core->basic_state() == 1 &&
                           core->has_putin_state() && core->putin_state() == 0;
    // Opposing core state is defined only when tier four is available.
    const bool opponent_known = core && core->has_enemy_core_status() && core->enemy_core_status() <= 2;
    const auto allowed_level = [&](std::uint32_t level) {
        if (!can_start || level > max_level) return false;
        if (max_level < 4) return true;
        if (!opponent_known || core->enemy_core_status() == 2) return false;
        return level < 4 || core->enemy_core_status() == 0;
    };
    if (levels_known && menu.selected_level > max_level) menu.selected_level = 1;
    ImGui::TextUnformatted("选择难度");
    const char *names[] = {"一级", "二级", "三级", "四级"};
    for (std::uint32_t level = 1; level <= 4; ++level) {
        ImGui::SameLine();
        ImGui::BeginDisabled(!allowed_level(level));
        if (ImGui::RadioButton(names[level - 1], menu.selected_level == level)) menu.selected_level = level;
        ImGui::EndDisabled();
    }
    // Protocol maximum is ten submissions/sec. A failed confirmation has a
    // separate one-second lockout in the rules, §5.4 (p.87).
    const bool paced = menu.last_sent == Clock::time_point{} || now - menu.last_sent >= 110ms;
    const bool confirm_ready = menu.last_confirm == Clock::time_point{} || now - menu.last_confirm >= 1s;
    const auto action = [&](const char *label, std::uint32_t operation, bool available) {
        ImGui::BeginDisabled(!paced || !available);
        const bool clicked = ImGui::Button(label);
        ImGui::EndDisabled();
        if (!clicked) return;
        AssemblyCommand request;
        request.set_operation(operation);
        if (operation == 0) request.set_difficulty(menu.selected_level);
        std::string payload;
        if (!request.SerializeToString(&payload)) return;
        client.publish("AssemblyCommand", payload);
        menu.last_request = label;
        if (operation == 0) menu.last_request += std::string(" · ") + names[menu.selected_level - 1];
        menu.last_sent = now;
        if (operation == 1) menu.last_confirm = now;
    };
    action("申请开始", 0, allowed_level(menu.selected_level));
    ImGui::SameLine();
    action("申请确认装配", 1, enabled && alive && confirm_ready && core->basic_state() == 3 &&
           core->has_putin_state() && core->putin_state() == 1);
    ImGui::SameLine();
    action("申请取消", 2, enabled && core->basic_state() != 1);
    if (!menu.last_request.empty()) {
        ImGui::TextWrapped("最近提交：%s  ·  %s", menu.last_request.c_str(),
            state.cup_core_received_at > menu.last_sent ? "后续场馆状态见上方" : "等待场馆状态更新");
    }
    if (!playing) ImGui::TextDisabled("比赛进行且未暂停时可以申请");
    else if (!core || !known) ImGui::TextDisabled("等待裁判下发核心位置");
    else if (!fresh) ImGui::TextDisabled("核心状态超过 3 秒未更新，等待新数据");
    else if (!alive) ImGui::TextDisabled("开始或确认装配前须收到本车存活状态");
    else if (core->basic_state() == 1 && max_level == 4 && (!opponent_known || core->enemy_core_status() == 2))
        ImGui::TextDisabled(opponent_known ? "对方四级装配进行中" : "等待对方核心状态");
    else if (core->basic_state() == 1) ImGui::TextDisabled("开始前须激活装配区，适用难度以裁判数据为准");
}
