#include "bottom_panel.h"

void ShowBottomPanel(const State &state) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGuiStyle &style = ImGui::GetStyle();
    float font_size = ImGui::GetFontSize();

    struct StatusLine {
        std::string text;
        ImVec4 color;
    };

    std::vector<StatusLine> status_lines;

    // 复活状态显示
    if (state.robot_respawn_status.has_value()) {
        const auto &respawn_status = *state.robot_respawn_status;
        if (respawn_status.is_pending_respawn()) {
            std::string respawn_text = "复活进度: ";
            respawn_text += std::to_string(respawn_status.current_respawn_progress());
            respawn_text += "/";
            respawn_text += std::to_string(respawn_status.total_respawn_progress());
            status_lines.push_back({std::move(respawn_text), ImVec4(1.0f, 1.0f, 0.3f, 1.0f)});
        }
    }

    // 装配状态显示
    if (state.tech_core_motion_state_sync.has_value()) {
        const auto &sync = *state.tech_core_motion_state_sync;

        // 判断是否在装配中：basic_state 为 2（运动中）或 3（已到达对应位姿）时表示在装配
        bool is_assembling = sync.basic_state() >= 2 && sync.basic_state() <= 3;

        if (is_assembling) {
            std::string status_text;

            // 根据 basic_state 构建状态描述
            if (sync.basic_state() == 2) {
                status_text = "科技核心移动中";
            } else if (sync.basic_state() == 3) {
                // 显示当前装配步骤状态
                std::vector<std::string> steps;
                if (sync.putin_state() == 1) steps.push_back("已放入");
                if (sync.move_state() == 1) steps.push_back("已平移");
                if (sync.rotate_state() == 1) steps.push_back("已旋转");

                if (steps.empty()) {
                    status_text = "等待装配";
                } else {
                    status_text = "装配中: " + std::accumulate(steps.begin(), steps.end(), std::string(),
                        [](const std::string &a, const std::string &b) {
                            return a.empty() ? b : a + " -> " + b;
                        });
                }

                // 显示剩余时间
                if (sync.remain_time_step() > 0) {
                    status_text += " (" + std::to_string(sync.remain_time_step()) + "s)";
                }
            }

            status_lines.push_back({"工程装配状态: " + status_text, ImVec4(0.3f, 0.8f, 1.0f, 1.0f)});
        }
    }

    if (state.air_support_status_sync.has_value()) {
        const auto &sync = *state.air_support_status_sync;
        std::string air_support_text = "空中支援: ";
        air_support_text += GetAirSupportStatusName(sync.airsupport_status());
        air_support_text += " | 免费剩余 ";
        air_support_text += std::to_string(sync.left_time());
        air_support_text += "s | 激光";
        air_support_text += (sync.is_being_targeted() != 0 ? "照射中" : "未照射");
        air_support_text += " | 发射机构";
        air_support_text += GetAirSupportShooterStatusName(sync.shooter_status());
        if (sync.cost_coins() > 0) {
            air_support_text += " | 已花费 ";
            air_support_text += std::to_string(sync.cost_coins());
            air_support_text += " 金币";
        }

        ImVec4 color = ImVec4(0.7f, 0.75f, 0.85f, 1.0f);
        if (sync.is_being_targeted() != 0 || sync.shooter_status() == 0 || sync.shooter_status() == 2) {
            color = ImVec4(1.0f, 0.55f, 0.25f, 1.0f);
        } else if (sync.airsupport_status() == 1) {
            color = ImVec4(0.2f, 0.85f, 1.0f, 1.0f);
        }
        status_lines.push_back({std::move(air_support_text), color});
    }

    const uint8_t air_support_counter_countdown =
            GetAirSupportCounterCooldownByte(state.air_support_custom_control, std::chrono::steady_clock::now());
    if (air_support_counter_countdown != kInactiveAirSupportCounterCooldown) {
        std::string counter_text = "空中支援反制倒计时: ";
        counter_text += std::to_string(air_support_counter_countdown);
        counter_text += "s";
        status_lines.push_back({std::move(counter_text), ImVec4(1.0f, 0.78f, 0.25f, 1.0f)});
    }

    size_t line_count = std::max<size_t>(1, status_lines.size());
    float panel_height = font_size * static_cast<float>(line_count)
                         + style.ItemSpacing.y * static_cast<float>(line_count - 1)
                         + style.WindowPadding.y * 2;

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;
    ImGui::SetNextWindowPos(ImVec2(0, viewport->Size.y - panel_height), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, panel_height), ImGuiCond_Always);

    if (!ImGui::Begin("BottomPanel", nullptr, window_flags)) {
        ImGui::End();
        return;
    }

    for (const auto &line: status_lines) {
        ImVec2 text_size = ImGui::CalcTextSize(line.text.c_str());
        ImGui::SetCursorPosX((viewport->Size.x - text_size.x) / 2.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, line.color);
        ImGui::Text("%s", line.text.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::End();
}
