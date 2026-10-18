#include "robot_status_panel.h"

void ShowProgressBar(const ImVec4 &bar_color, float ratio, float width, float height) {
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar_color);
    ImGui::ProgressBar(ratio, ImVec2(width, height), "");
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

void ShowRobotStatusPanel(const State &state) {
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("VehicleStatusPanel", nullptr, window_flags)) {
        ImGui::End();
        return;
    }

    // 获取可用内容区域大小
    ImVec2 content_avail = ImGui::GetContentRegionAvail();
    float progress_bar_height = 4.0f;

    // Panel 标题
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::Text("机器人状态");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // TODO show buffer energy bar
    uint32_t current_health, max_health, current_energy, max_energy, current_experience, experience_for_upgrade, level;
    float health_ratio, energy_ratio, experience_ratio;
    if (state.robot_static_status.has_value() && state.robot_dynamic_status.has_value()) {
        current_health = state.robot_dynamic_status->current_health();
        max_health = state.robot_static_status->max_health();
        health_ratio = static_cast<float>(current_health) / static_cast<float>(max_health);
        current_energy = state.robot_dynamic_status->current_chassis_energy();
        max_energy = state.robot_static_status->max_chassis_energy();
        energy_ratio = static_cast<float>(current_energy) / static_cast<float>(max_energy);
        current_experience = state.robot_dynamic_status->current_experience();
        experience_for_upgrade = state.robot_dynamic_status->experience_for_upgrade();
        level = state.robot_static_status->level();
        experience_ratio = static_cast<float>(current_experience) / static_cast<float>(experience_for_upgrade);
    } else {
        current_health = 0;
        max_health = 0;
        current_energy = 0;
        max_energy = 0;
        current_experience = 0;
        experience_for_upgrade = 0;
        level = 0;
        health_ratio = 0.0f;
        energy_ratio = 0.0f;
        experience_ratio = 0.0f;
    }

    // 血条
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
    ImGui::Text("血量: %d/%d", current_health, max_health);
    ImGui::PopStyleColor();
    ShowProgressBar(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), health_ratio, content_avail.x, progress_bar_height);

    // 能量条
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.7f, 1.0f, 1.0f));
    ImGui::Text("能量: %d/%d", current_energy, max_energy);
    ImGui::PopStyleColor();
    ShowProgressBar(ImVec4(0.3f, 0.7f, 1.0f, 1.0f), energy_ratio, content_avail.x, progress_bar_height);

    // 经验条
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.3f, 1.0f));
    ImGui::Text("经验: %d/%d (等级: %d)", current_experience, experience_for_upgrade, level);
    ImGui::PopStyleColor();
    ShowProgressBar(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), experience_ratio, content_avail.x, progress_bar_height);

    // 允许发弹量
    uint32_t remaining_ammo;
    if (state.robot_dynamic_status.has_value()) {
        remaining_ammo = state.robot_dynamic_status->remaining_ammo();
    } else {
        remaining_ammo = 0;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.8f, 1.0f, 1.0f));
    ImGui::Text("允许发弹量: %d", remaining_ammo);
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // 指示灯
    bool is_auto_aim = state.auto_aim;
    bool is_gyro = state.gyro;
    bool is_deploy_mode;
    if (state.deploy_mode_status_sync.has_value()) {
        is_deploy_mode = state.deploy_mode_status_sync->status() == 1;
    } else {
        is_deploy_mode = false;
    }

    float text_width1 = ImGui::CalcTextSize("指示灯: ").x;
    float text_width2 = ImGui::CalcTextSize("自瞄").x;
    float text_width3 = ImGui::CalcTextSize("小陀螺").x;
    float text_width4 = ImGui::CalcTextSize("部署模式").x;
    float indicator_spacing = (content_avail.x - text_width1 - text_width2 - text_width3 - text_width4) / 4.0f;
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "指示灯: ");
    ImGui::SameLine(text_width1 + indicator_spacing);
    ImGui::TextColored(is_auto_aim ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f) : ImVec4(0.5, 0.5, 0.5, 1), "自瞄");
    ImGui::SameLine(text_width1 + text_width2 + indicator_spacing * 2);
    ImGui::TextColored(is_gyro ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f) : ImVec4(0.5, 0.5, 0.5, 1), "小陀螺");
    ImGui::SameLine(text_width1 + text_width2 + text_width3 + indicator_spacing * 3);
    ImGui::TextColored(is_deploy_mode ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f) : ImVec4(0.5, 0.5, 0.5, 1), "部署模式");
    ImGui::Spacing();

    // 设备连接状态
    const char *module_names[] = {
            "主控", "电源管理", "图传", "RFID", "UWB",
            "装甲板", "小弹丸", "大弹丸", "超级电容", "灯条",
            "激光检测"};

    ImGui::SameLine(0, 0);
    ImGui::TextColored(ImVec4(0.8f, 0.4f, 1.0f, 1.0f), "未连接设备: ");

    if (state.robot_module_status.has_value()) {
        uint32_t module_status[] = {
                state.robot_module_status->main_controller(),
                state.robot_module_status->power_manager(),
                state.robot_module_status->video_transmission(),
                state.robot_module_status->rfid(),
                state.robot_module_status->uwb(),
                state.robot_module_status->armor(),
                state.robot_module_status->small_shooter(),
                state.robot_module_status->big_shooter(),
                state.robot_module_status->capacitor(),
                state.robot_module_status->light_strip(),
                state.robot_module_status->laser_detection_module()};
        static_assert(std::size(module_names) == std::size(module_status));

        ImGui::SameLine(0, 0);
        ImGuiTextBuffer buffer;
        for (size_t i = 0; i < std::size(module_names); i++) {
            if (module_status[i] != 1) {
                if (!buffer.empty()) {
                    buffer.append(", ");
                }
                buffer.appendf("%s", module_names[i]);
            }
        }
        if (buffer.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "（无）");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", buffer.c_str());
        }
    } else {
        ImGui::SameLine(0, 0);
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "未知");
    }

    ImGui::End();
}
