#include "buff_panel.h"

std::string GetBuffName(uint32_t buff_type, int32_t buff_level) {
    std::string name;
    switch (buff_type) {
        case 1:
            name = "攻击增益";
            break;
        case 2:
            name = buff_level >= 0 ? "防御增益" : "负防御增益";
            break;
        case 3:
            name = "射击热量冷却增益";
            break;
        case 4:
            name = "底盘功率增益";
            break;
        case 5:
            name = "回血增益";
            break;
        case 6:
            return "可兑换允许发弹量";
        case 7:
            return "地形跨越增益";
        default:
            return "未知增益";
    }

    int level = std::abs(buff_level);
    name.push_back(' ');
    name.push_back(buff_level >= 0 ? '+' : '-');
    name.append(std::to_string(level));
    if (buff_type == 3) {
        // 射击热量冷却增益 - 直接值
        name.append("/s");
    } else {
        // 百分比类型
        name.push_back('%');
    }

    return name;
}

ImVec4 GetBuffTypeColor(uint32_t buff_type) {
    switch (buff_type) {
        case 1:// 攻击增益 - 红色
            return {1.0f, 0.3f, 0.3f, 1.0f};
        case 2:// 防御增益 - 蓝色
            return {0.3f, 0.5f, 1.0f, 1.0f};
        case 3:// 射击热量冷却增益 - 橙色
            return {1.0f, 0.7f, 0.2f, 1.0f};
        case 4:// 底盘功率增益 - 黄色
            return {1.0f, 1.0f, 0.3f, 1.0f};
        case 5:// 回血增益 - 绿色
            return {0.3f, 1.0f, 0.3f, 1.0f};
        case 6:// 可兑换允许发弹量 - 紫色
            return {0.7f, 0.3f, 1.0f, 1.0f};
        case 7:// 地形跨越增益 - 青色
            return {0.3f, 1.0f, 1.0f, 1.0f};
        default:
            return {0.7f, 0.7f, 0.7f, 1.0f};
    }
}

void ShowBuffPanel(const State &state) {
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration |
                                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
                                    ImGuiWindowFlags_NoSavedSettings;

    if (!ImGui::Begin("BuffPanel", nullptr, window_flags)) {
        ImGui::End();
        return;
    }

    ImVec2 content_avail = ImGui::GetContentRegionAvail();
    float progress_bar_height = 3.0f;

    // Panel title
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Buff 状态");
    ImGui::Separator();

    ImGui::Spacing();

    // Get current robot ID
    uint32_t current_robot_id = state.robot_static_status.has_value() ? state.robot_static_status->robot_id() : 0;

    if (state.active_buffs.empty()) {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "无激活的 Buff");
        ImGui::End();
        return;
    }

    // Get current time for countdown
    auto now = std::chrono::steady_clock::now();

    // Display each buff for current robot only
    for (const auto &buff_entry: state.active_buffs) {
        const Buff &buff = buff_entry.buff;

        // Only show buffs for current robot
        if (buff.robot_id() != current_robot_id) {
            continue;
        }

        std::string buff_name = GetBuffName(buff.buff_type(), buff.buff_level());
        ImVec4 buff_color = GetBuffTypeColor(buff.buff_type());

        // Calculate elapsed time and remaining time
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - buff_entry.update_time).count();
        int remaining_time = static_cast<int>(buff.buff_left_time()) - static_cast<int>(elapsed);

        // Skip if expired (should have been removed above, but double check)
        if (remaining_time <= 0) {
            continue;
        }

        float time_ratio = static_cast<float>(remaining_time) /
                           static_cast<float>(buff.buff_max_time());

        // Buff name with level included in the name
        ImGui::TextColored(buff_color, "%s", buff_name.c_str());

        // Time progress bar
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, buff_color);
        ImGui::ProgressBar(time_ratio, ImVec2(content_avail.x, progress_bar_height), "");
        ImGui::PopStyleColor();

        // Time text
        ImGui::Text("%d/%d 秒", remaining_time, buff.buff_max_time());

        ImGui::Spacing();
    }

    ImGui::End();
}
