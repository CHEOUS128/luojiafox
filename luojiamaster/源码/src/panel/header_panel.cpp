#include "header_panel.h"

void AddTextInTheCenterOfRect(ImDrawList *draw_list, const char *text_begin, const char *text_end, float left, float top, float right, float bottom, ImU32 col) {
    ImVec2 text_size = ImGui::CalcTextSize(text_begin, text_end);
    draw_list->AddText(ImVec2(left + (right - left) / 2 - text_size.x / 2, top + (bottom - top) / 2 - text_size.y / 2), col, text_begin, text_end);
}

const char *GetGameResultName(uint32_t game_result) {
    switch (game_result) {
        case 0:
            return "平局";
        case 1:
            return "红方胜利";
        case 2:
            return "蓝方胜利";
        default:
            return nullptr;
    }
}

void ShowHeaderPanel(const State &state) {
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("HeaderWindow", nullptr, window_flags)) {
        ImGui::End();
        return;
    }
    const ImVec2 window_pos = ImGui::GetWindowPos();
    const ImVec2 window_size = ImGui::GetWindowSize();
    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    const char *text_begin, *text_end;

    uint32_t current_stage;
    int32_t stage_countdown_sec;
    bool is_paused;
    if (state.game_status.has_value()) {
        current_stage = state.game_status->current_stage();
        stage_countdown_sec = state.game_status->stage_countdown_sec();
        is_paused = state.game_status->is_paused();
        if (current_stage == 0) {
            stage_countdown_sec = 0;
        }
    } else {
        current_stage = 0;
        stage_countdown_sec = 0;
        is_paused = false;
    }

    // 比赛阶段显示
    ImGui::PushFont(ImGui::GetFont(), 14);
    const char *stage_text;
    std::string stage_text_storage;
    if (state.game_status.has_value()) {
        if (is_paused) {
            stage_text = "技术暂停中";
        } else {
            switch (current_stage) {
                case 0:
                    stage_text = "未开始比赛";
                    break;
                case 1:
                    stage_text = "准备阶段";
                    break;
                case 2:
                    stage_text = "裁判系统自检";
                    break;
                case 3:
                    stage_text = "五秒倒计时";
                    break;
                case 4:
                    stage_text = "比赛中";
                    break;
                case 5:
                    if (state.game_status->has_game_result()) {
                        const char *game_result_name = GetGameResultName(state.game_status->game_result());
                        if (game_result_name != nullptr) {
                            stage_text_storage = std::string("比赛结算: ") + game_result_name;
                            stage_text = stage_text_storage.c_str();
                            break;
                        }
                    }
                    stage_text = "比赛结算中";
                    break;
                default:
                    stage_text = "未知阶段";
                    break;
            }
        }
    } else {
        stage_text = "未连接";
    }
    ImVec2 stage_text_size = ImGui::CalcTextSize(stage_text);
    float stage_left = window_pos.x + (window_size.x - stage_text_size.x) / 2;
    float stage_top = window_pos.y + 12 * ImGui::GetScale() + 5 * ImGui::GetScale();
    draw_list->AddText(ImVec2(stage_left, stage_top), IM_COL32(255, 255, 255, 255), stage_text);
    ImGui::PopFont();

    // 倒计时
    ImGui::PushFont(ImGui::GetFont(), 22);
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%02d:%02d", stage_countdown_sec / 60, stage_countdown_sec % 60);
    ImVec2 countdown_text_size = ImGui::CalcTextSize(text_begin, text_end);
    float countdown_left = window_pos.x + (window_size.x - countdown_text_size.x) / 2;
    float countdown_right = countdown_left + countdown_text_size.x;
    float countdown_top = stage_top + stage_text_size.y + 10 * ImGui::GetScale();
    float countdown_bottom = countdown_top + 20 * ImGui::GetScale();
    draw_list->AddText(ImVec2(countdown_left, countdown_top + (countdown_bottom - countdown_top - countdown_text_size.y) / 2), IM_COL32(255, 255, 255, 255), text_begin, text_end);
    ImGui::PopFont();

    // 金币显示
    uint32_t gold = state.global_logistics_status.has_value() ? state.global_logistics_status->remaining_economy() : 0;
    ImGui::PushFont(ImGui::GetFont(), 18);
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%d", gold);
    draw_list->AddText(ImVec2(window_pos.x + (window_size.x - ImGui::CalcTextSize(text_begin, text_end).x) / 2, countdown_bottom + 10 * ImGui::GetScale()), IM_COL32(255, 215, 0, 255), text_begin);
    ImGui::PopFont();

    // 前哨站血条和基地血条
    ImU32 red_color = IM_COL32(250, 63, 64, 255);
    ImU32 blue_color = IM_COL32(3, 187, 245, 255);
    ImU32 red_color_translated = IM_COL32(250, 63, 64, 50);
    ImU32 blue_color_translated = IM_COL32(3, 187, 245, 50);
    float rounding = 15 * ImGui::GetScale();
    float base_health_horizontal_padding = 24 * ImGui::GetScale();
    float outpost_length_ratio = 0.2f;
    float building_health_top = window_pos.y + 12 * ImGui::GetScale();
    float building_health_bottom = building_health_top + 30 * ImGui::GetScale();
    float red_health_left = window_pos.x + base_health_horizontal_padding;
    float red_health_right = countdown_left - base_health_horizontal_padding;
    float blue_health_left = countdown_right + base_health_horizontal_padding;
    float blue_health_right = window_pos.x + window_size.x - base_health_horizontal_padding;
    float red_outpost_health_left = red_health_left;
    float red_outpost_health_right = red_health_left + (red_health_right - red_health_left) * outpost_length_ratio - base_health_horizontal_padding / 2;
    float red_base_health_left = red_health_left + (red_health_right - red_health_left) * outpost_length_ratio + base_health_horizontal_padding / 2;
    float red_base_health_right = red_health_right;
    float blue_outpost_health_left = blue_health_right - (blue_health_right - blue_health_left) * outpost_length_ratio + base_health_horizontal_padding / 2;
    float blue_outpost_health_right = blue_health_right;
    float blue_base_health_left = blue_health_left;
    float blue_base_health_right = blue_health_right - (blue_health_right - blue_health_left) * outpost_length_ratio - base_health_horizontal_padding / 2;
    uint32_t outpost_max_health = 1500;
    uint32_t base_max_health = 5000;
    uint32_t red_outpost_health, red_base_health, red_base_shield, blue_outpost_health, blue_base_health, blue_base_shield;
    if (state.global_unit_status.has_value() && state.robot_static_status.has_value()) {
        if (state.robot_static_status->robot_id() < 100) {
            red_outpost_health = state.global_unit_status->outpost_health();
            red_base_health = state.global_unit_status->base_health();
            red_base_shield = state.global_unit_status->base_shield();
            blue_outpost_health = state.global_unit_status->enemy_outpost_health();
            blue_base_health = state.global_unit_status->enemy_base_health();
            blue_base_shield = state.global_unit_status->enemy_base_shield();
        } else {
            red_outpost_health = state.global_unit_status->enemy_outpost_health();
            red_base_health = state.global_unit_status->enemy_base_health();
            red_base_shield = state.global_unit_status->enemy_base_shield();
            blue_outpost_health = state.global_unit_status->outpost_health();
            blue_base_health = state.global_unit_status->base_health();
            blue_base_shield = state.global_unit_status->base_shield();
        }
    } else {
        red_outpost_health = 0;
        red_base_health = 0;
        red_base_shield = 0;
        blue_outpost_health = 0;
        blue_base_health = 0;
        blue_base_shield = 0;
    }

    // 绘制血条背景
    draw_list->AddRectFilled(ImVec2(red_outpost_health_left, building_health_top), ImVec2(red_outpost_health_right, building_health_bottom), red_color_translated, rounding, ImDrawFlags_RoundCornersLeft);
    draw_list->AddRectFilled(ImVec2(red_base_health_left, building_health_top), ImVec2(red_base_health_right, building_health_bottom), red_color_translated, rounding, ImDrawFlags_RoundCornersLeft);
    draw_list->AddRectFilled(ImVec2(blue_outpost_health_left, building_health_top), ImVec2(blue_outpost_health_right, building_health_bottom), blue_color_translated, rounding, ImDrawFlags_RoundCornersRight);
    draw_list->AddRectFilled(ImVec2(blue_base_health_left, building_health_top), ImVec2(blue_base_health_right, building_health_bottom), blue_color_translated, rounding, ImDrawFlags_RoundCornersRight);

    // 绘制血条填充
    if (red_outpost_health > 0) {
        draw_list->AddRectFilled(ImVec2(red_outpost_health_left + (red_outpost_health_right - red_outpost_health_left) * (1 - static_cast<float>(red_outpost_health) / static_cast<float>(outpost_max_health)), building_health_top), ImVec2(red_outpost_health_right, building_health_bottom), red_color, rounding, ImDrawFlags_RoundCornersLeft);
    }
    if (red_base_health > 0) {
        draw_list->AddRectFilled(ImVec2(red_base_health_left + (red_base_health_right - red_base_health_left) * (1 - static_cast<float>(red_base_health) / static_cast<float>(base_max_health)), building_health_top), ImVec2(red_base_health_right, building_health_bottom), red_color, rounding, ImDrawFlags_RoundCornersLeft);
    }
    if (blue_outpost_health > 0) {
        draw_list->AddRectFilled(ImVec2(blue_outpost_health_left, building_health_top), ImVec2(blue_outpost_health_left + (blue_outpost_health_right - blue_outpost_health_left) * (static_cast<float>(blue_outpost_health) / static_cast<float>(outpost_max_health)), building_health_bottom), blue_color, rounding, ImDrawFlags_RoundCornersRight);
    }
    if (blue_base_health > 0) {
        draw_list->AddRectFilled(ImVec2(blue_base_health_left, building_health_top), ImVec2(blue_base_health_left + (blue_base_health_right - blue_base_health_left) * (static_cast<float>(blue_base_health) / static_cast<float>(base_max_health)), building_health_bottom), blue_color, rounding, ImDrawFlags_RoundCornersRight);
    }

    // 绘制血条边框
    draw_list->AddRect(ImVec2(red_outpost_health_left, building_health_top), ImVec2(red_outpost_health_right, building_health_bottom), red_color, rounding, 1.0f, ImDrawFlags_RoundCornersLeft);
    draw_list->AddRect(ImVec2(red_base_health_left, building_health_top), ImVec2(red_base_health_right, building_health_bottom), red_color, rounding, 1.0f, ImDrawFlags_RoundCornersLeft);
    draw_list->AddRect(ImVec2(blue_outpost_health_left, building_health_top), ImVec2(blue_outpost_health_right, building_health_bottom), blue_color, rounding, 1.0f, ImDrawFlags_RoundCornersRight);
    draw_list->AddRect(ImVec2(blue_base_health_left, building_health_top), ImVec2(blue_base_health_right, building_health_bottom), blue_color, rounding, 1.0f, ImDrawFlags_RoundCornersRight);

    // 血条文本
    ImGui::PushFont(ImGui::GetFont(), 16);
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%d/%d", red_outpost_health, outpost_max_health);
    AddTextInTheCenterOfRect(draw_list, text_begin, text_end, red_outpost_health_left, building_health_top, red_outpost_health_right, building_health_bottom, IM_COL32(255, 255, 255, 255));
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%d/%d", red_base_health + red_base_shield, base_max_health);
    AddTextInTheCenterOfRect(draw_list, text_begin, text_end, red_base_health_left, building_health_top, red_base_health_right, building_health_bottom, IM_COL32(255, 255, 255, 255));
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%d/%d", blue_outpost_health, outpost_max_health);
    AddTextInTheCenterOfRect(draw_list, text_begin, text_end, blue_outpost_health_left, building_health_top, blue_outpost_health_right, building_health_bottom, IM_COL32(255, 255, 255, 255));
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%d/%d", blue_base_health + blue_base_shield, base_max_health);
    AddTextInTheCenterOfRect(draw_list, text_begin, text_end, blue_base_health_left, building_health_top, blue_base_health_right, building_health_bottom, IM_COL32(255, 255, 255, 255));
    ImGui::PopFont();

    // 机器人血量条
    float robot_bar_top = building_health_bottom + 30 * ImGui::GetScale();
    float robot_bar_bottom = robot_bar_top + 4 * ImGui::GetScale();
    float red_robot_bar_left = red_health_left;
    float red_robot_bar_right = red_health_right;
    float blue_robot_bar_left = blue_health_left;
    float blue_robot_bar_right = blue_health_right;
    const char *robot_names[] = {"英雄", "工程", "步兵", "步兵", "步兵", "无人机", "哨兵", "飞镖", "雷达"};
    static_assert(sizeof(state.red_robots) / sizeof(state.red_robots[0]) == sizeof(robot_names) / sizeof(robot_names[0]));
    int ids[] = {1, 2, 3, 4, 6, 7, 8, 9};
    int ids_size = std::size(ids);
    float robot_bar_spacing = 6.0f * ImGui::GetScale();
    ImU32 bg_color = IM_COL32(50, 50, 50, 255);
    float red_robot_bar_item_width = (red_robot_bar_right - red_robot_bar_left - robot_bar_spacing * static_cast<float>(ids_size - 1)) / static_cast<float>(ids_size);
    float blue_robot_bar_item_width = (blue_robot_bar_right - blue_robot_bar_left - robot_bar_spacing * static_cast<float>(ids_size - 1)) / static_cast<float>(ids_size);
    ImGui::PushFont(ImGui::GetFont(), 12);
    for (int i = 0; i < ids_size; i++) {
        int id = ids[i];
        const RobotStatus &robot = state.red_robots[id - 1];
        float bar_left = red_robot_bar_left + static_cast<float>(i) * (red_robot_bar_item_width + robot_bar_spacing);
        float bar_right = bar_left + red_robot_bar_item_width;
        bool has_health = id != 6 && id != 8 && id != 9;// 无人机，飞镖，雷达站没有血条
        bool show_health = has_health && robot.max_health > 0;
        draw_list->AddRectFilled(ImVec2(bar_left, robot_bar_top), ImVec2(bar_right, robot_bar_bottom), bg_color, 2.0f);
        if (show_health) {
            float ratio = static_cast<float>(robot.health) / static_cast<float>(robot.max_health);
            if (ratio > 1.0f) {
                ratio = 1.0f;
            }
            if (ratio > 0.0f) {
                draw_list->AddRectFilled(ImVec2(bar_left, robot_bar_top), ImVec2(bar_left + (bar_right - bar_left) * ratio, robot_bar_bottom), red_color, 2.0f);
            }
        }
        ImFormatStringToTempBuffer(&text_begin, &text_end, "%d %s", id, robot_names[id - 1]);
        ImVec2 id_size = ImGui::CalcTextSize(text_begin, text_end);
        draw_list->AddText(ImVec2(bar_left + (red_robot_bar_item_width - id_size.x) / 2, robot_bar_top - 2 - id_size.y), IM_COL32(255, 255, 255, 255), text_begin, text_end);
        if (show_health) {
            ImFormatStringToTempBuffer(&text_begin, &text_end, "%d/%d", robot.health, robot.max_health);
        } else {
            ImFormatStringToTempBuffer(&text_begin, &text_end, "-");
        }
        ImVec2 health_size = ImGui::CalcTextSize(text_begin, text_end);
        float health_text_y = robot_bar_bottom + 2;
        draw_list->AddText(ImVec2(bar_left + (red_robot_bar_item_width - health_size.x) / 2, health_text_y), IM_COL32(255, 255, 255, 255), text_begin, text_end);
        // 显示允许发弹量
        ImFormatStringToTempBuffer(&text_begin, &text_end, "弹药:%d", robot.bullets);
        ImVec2 bullets_size = ImGui::CalcTextSize(text_begin, text_end);
        draw_list->AddText(ImVec2(bar_left + (red_robot_bar_item_width - bullets_size.x) / 2, health_text_y + health_size.y + 1), IM_COL32(200, 200, 200, 255), text_begin, text_end);
    }
    for (int i = 0; i < ids_size; i++) {
        int id = ids[i];
        const RobotStatus &robot = state.blue_robots[id - 1];
        float bar_left = blue_robot_bar_left + static_cast<float>(i) * (blue_robot_bar_item_width + robot_bar_spacing);
        float bar_right = bar_left + blue_robot_bar_item_width;
        bool has_health = id != 6 && id != 8 && id != 9;// 无人机，飞镖，雷达站没有血条
        bool show_health = has_health && robot.max_health > 0;
        draw_list->AddRectFilled(ImVec2(bar_left, robot_bar_top), ImVec2(bar_right, robot_bar_bottom), bg_color, 2.0f);
        if (show_health) {
            float ratio = static_cast<float>(robot.health) / static_cast<float>(robot.max_health);
            if (ratio > 1.0f) {
                ratio = 1.0f;
            }
            if (ratio > 0.0f) {
                draw_list->AddRectFilled(ImVec2(bar_left, robot_bar_top), ImVec2(bar_left + (bar_right - bar_left) * ratio, robot_bar_bottom), blue_color, 2.0f);
            }
        }
        ImFormatStringToTempBuffer(&text_begin, &text_end, "%d %s", id, robot_names[id - 1]);
        ImVec2 id_size = ImGui::CalcTextSize(text_begin, text_end);
        draw_list->AddText(ImVec2(bar_left + (blue_robot_bar_item_width - id_size.x) / 2, robot_bar_top - 2 - id_size.y), IM_COL32(255, 255, 255, 255), text_begin, text_end);
        if (show_health) {
            ImFormatStringToTempBuffer(&text_begin, &text_end, "%d/%d", robot.health, robot.max_health);
        } else {
            ImFormatStringToTempBuffer(&text_begin, &text_end, "-");
        }
        ImVec2 health_size = ImGui::CalcTextSize(text_begin, text_end);
        float health_text_y = robot_bar_bottom + 2;
        draw_list->AddText(ImVec2(bar_left + (blue_robot_bar_item_width - health_size.x) / 2, health_text_y), IM_COL32(255, 255, 255, 255), text_begin, text_end);
        // 显示允许发弹量
        ImFormatStringToTempBuffer(&text_begin, &text_end, "弹药:%d", robot.bullets);
        ImVec2 bullets_size = ImGui::CalcTextSize(text_begin, text_end);
        draw_list->AddText(ImVec2(bar_left + (blue_robot_bar_item_width - bullets_size.x) / 2, health_text_y + health_size.y + 1), IM_COL32(200, 200, 200, 255), text_begin, text_end);
    }
    ImGui::PopFont();

    ImGui::End();
}
