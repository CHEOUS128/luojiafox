#pragma once

#include "app/match_values.h"
#include "main_window.h"

namespace RoboGo::app {
inline void DrawMatchStrip(const State &state) {
    const auto &game = state.game_status;
    if (game && game->has_current_round()) ImGui::Text("第 %u 局", game->current_round());
    else ImGui::TextUnformatted("局数待定");
    ImGui::SameLine();
    const char *phase = "等待赛况";
    if (game && game->has_current_stage()) {
        constexpr std::array<const char *,6> names{"未开始","准备","自检","开赛倒计时","比赛中","结算"};
        if (game->current_stage() < names.size()) phase = names[game->current_stage()];
    }
    ImGui::TextUnformatted(game && game->has_is_paused() && game->is_paused() ? "已暂停" : phase);
    ImGui::SameLine();
    if (game && game->has_stage_countdown_sec() && game->stage_countdown_sec() >= 0) {
        const int sec = game->stage_countdown_sec();
        ImGui::Text("%02d:%02d", sec / 60, sec % 60);
    } else ImGui::TextDisabled("时间待定");
    for (bool blue : {false,true}) {
        ImGui::TextColored(blue ? ImVec4(.48f,.78f,1.f,1.f) : ImVec4(1.f,.65f,.6f,1.f),
                           "%s", blue ? "蓝方血量" : "红方血量");
        for (int role : {1,3,7}) {
            ImGui::SameLine();
            const auto hp = Health2027(state.robot_static_status, state.global_unit_status, blue, role);
            if (hp) ImGui::Text("%d号 %u",role,*hp);
            else ImGui::TextDisabled("%d号 等待", role);
        }
    }
}
} // namespace RoboGo::app
