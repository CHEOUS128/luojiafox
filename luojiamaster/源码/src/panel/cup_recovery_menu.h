#pragma once

#include "main_window.h"

namespace luojiamaster::cup_menu {

struct CupRecoveryUi {
    uint64_t generation = 0;
    std::string seat;
    std::string last_request;
    std::chrono::steady_clock::time_point last_click{};
};

namespace recovery {

// RMUC table 5-7 rounds after multiplying by 20. At 419/360/359 s
// remaining the price is respectively 51/70/71 coins.
inline std::optional<uint32_t> HealPrice(const std::optional<GameStatus>& game) {
    if (!game || !game->has_current_stage() || game->current_stage() != 4 ||
        !game->has_stage_countdown_sec()) return std::nullopt;
    const int left = game->stage_countdown_sec();
    if (left < 0 || left > 420) return std::nullopt;
    return 50u + static_cast<uint32_t>(((420 - left) * 20 + 59) / 60);
}

inline const char* MatchGate(const State& state, const std::string& seat, bool connected) {
    if (!connected) return "等待连接";
    const auto& self = state.robot_static_status;
    if (!self || !self->has_robot_id() || std::to_string(self->robot_id()) != seat)
        return "等待本车身份";
    switch (self->robot_id()) {
        case 1: case 3: case 4: case 101: case 103: case 104: break;
        default: return "仅限重装和步兵岗位";
    }
    if (self->has_connection_state() && self->connection_state() != 1) return "本车未连接";
    if (self->has_field_state() && self->field_state() != 0) return "本车未上场";
    const auto& game = state.game_status;
    if (!game || !game->has_current_stage()) return "等待比赛阶段";
    if (std::chrono::steady_clock::now() - state.cup_clock_received_at >= std::chrono::seconds(4))
        return "等待比赛阶段更新";
    if (game->current_stage() != 4) return "比赛进行时可申请";
    if (!game->has_is_paused()) return "等待比赛暂停状态";
    if (game->is_paused()) return "比赛暂停中";
    return nullptr;
}

inline std::string Number(std::optional<uint32_t> value) {
    return value ? std::to_string(*value) : "等待";
}

inline const char* CostGate(std::optional<uint32_t> cost, std::optional<uint32_t> coins) {
    if (!cost) return "等待价格";
    if (!coins) return "等待己方金币";
    return *coins < *cost ? "己方金币不足" : nullptr;
}

inline void Request(const char* label, uint32_t command, const char* reason,
                    RoboGo::io::MqttClient& client, CupRecoveryUi& ui) {
    // Ignore a double click on the same purchase while waiting for new state.
    const auto now = std::chrono::steady_clock::now();
    const bool just_clicked = ui.last_click != std::chrono::steady_clock::time_point{} &&
                              now - ui.last_click < std::chrono::seconds(1);
    ImGui::BeginDisabled(reason || just_clicked);
    const bool pressed = ImGui::Button(label);
    ImGui::EndDisabled();
    if (reason) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", reason);
    }
    if (pressed && !reason && !just_clicked) {
        CommonCommand request;
        request.set_cmd_type(command);
        request.set_param(0);
        client.publish("CommonCommand", request.SerializeAsString());
        ui.last_request = label;
        ui.last_click = now;
    }
}

} // namespace recovery

inline void DrawCupRecovery(const State& state, RoboGo::io::MqttClient& client,
                            CupRecoveryUi& ui, bool local_trial) {
    if (state.competition != RoboGo::app::Competition::Uc2027) return;
    const auto seat = client.get_client_id();
    if (ui.generation != state.round_generation || ui.seat != seat) {
        ui = {};
        ui.generation = state.round_generation;
        ui.seat = seat;
    }
    using namespace recovery;
    const auto& body = state.robot_dynamic_status;
    const auto& self = state.robot_static_status;
    const auto& respawn = state.robot_respawn_status;
    const auto& team = state.global_logistics_status;
    const std::optional<uint32_t> hp = body && body->has_current_health()
        ? std::optional{body->current_health()} : std::nullopt;
    const std::optional<uint32_t> coins = team && team->has_remaining_economy()
        ? std::optional{team->remaining_economy()} : std::nullopt;
    const bool know_pending = respawn && respawn->has_is_pending_respawn();
    const bool pending = know_pending && respawn->is_pending_respawn();
    const bool connected = client.get_connection_state() == RoboGo::io::MqttClient::ConnectionState::Connected;
    const char* gate = MatchGate(state, seat, connected);

    ImGui::SeparatorText("赛中回血及复活");
    ImGui::TextWrapped("本车血量  %s     己方金币  %s     复活状态  %s", Number(hp).c_str(),
                       Number(coins).c_str(), know_pending ? (pending ? "待复活" : "未进入复活") : "等待");
    if (!pending) {
        const auto price = HealPrice(state.game_status);
        const char* reason = gate;
        if (!reason && (!body || !body->has_can_remote_heal())) reason = "等待回血许可";
        if (!reason && !body->can_remote_heal()) reason = "当前不能远程回血";
        if (!reason && body->has_is_out_of_combat() && !body->is_out_of_combat()) reason = "尚未脱战";
        if (!reason && ((hp && *hp == 0) || (self->has_alive_state() && self->alive_state() == 2)))
            reason = "本车已战亡";
        if (!reason) reason = CostGate(price, coins);
        ImGui::Text("远程回血  %s 金币", Number(price).c_str());
        Request("申请远程回血", 6, reason, client, ui);
    } else {
        const std::string current = respawn->has_current_respawn_progress()
            ? std::to_string(respawn->current_respawn_progress()) : "等待";
        const std::string total = respawn->has_total_respawn_progress()
            ? std::to_string(respawn->total_respawn_progress()) : "等待";
        const std::optional<uint32_t> price = respawn->has_gold_cost_for_respawn()
            ? std::optional{respawn->gold_cost_for_respawn()} : std::nullopt;
        ImGui::TextWrapped("复活读条  %s / %s     立即复活  %s 金币", current.c_str(),
                           total.c_str(), Number(price).c_str());
        const char* free_reason = gate;
        if (!free_reason && !respawn->has_can_free_respawn()) free_reason = "等待免费复活许可";
        if (!free_reason && !respawn->can_free_respawn()) free_reason = "当前不能免费复活";
        const char* paid_reason = gate;
        if (!paid_reason && !respawn->has_can_pay_for_respawn()) paid_reason = "等待付费复活许可";
        if (!paid_reason && !respawn->can_pay_for_respawn()) paid_reason = "当前不能付费复活";
        if (!paid_reason) paid_reason = CostGate(price, coins);
        Request("申请免费复活", 3, free_reason, client, ui);
        Request("申请立即复活", 4, paid_reason, client, ui);
    }
    if (!ui.last_request.empty()) {
        if (local_trial) ImGui::TextWrapped("本地已记录“%s”。血量和复活状态按样例播放。", ui.last_request.c_str());
        else ImGui::TextWrapped("上次操作：%s。当前血量和复活状态以裁判同步为准。", ui.last_request.c_str());
    }
}

} // namespace luojiamaster::cup_menu
