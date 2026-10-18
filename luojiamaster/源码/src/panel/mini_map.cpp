#include "mini_map.h"
#include "keyboard_mouse_state_manager.h"

namespace {

constexpr float kMapWidth = 280.0f;
constexpr float kMapHeight = 150.0f;

constexpr ImU32 kTeamRedColor = IM_COL32(250, 63, 64, 255);
constexpr ImU32 kTeamBlueColor = IM_COL32(3, 187, 245, 255);
constexpr ImU32 kTeamRedFillColor = IM_COL32(250, 63, 64, 72);
constexpr ImU32 kTeamBlueFillColor = IM_COL32(3, 187, 245, 72);

constexpr ImU32 kMapBackgroundColor = IM_COL32(24, 24, 28, 235);
constexpr ImU32 kMapOverlayColor = IM_COL32(12, 22, 30, 72);
constexpr ImU32 kMapInnerBorderRedColor = IM_COL32(250, 63, 64, 52);
constexpr ImU32 kMapInnerBorderBlueColor = IM_COL32(92, 175, 236, 52);
constexpr ImU32 kMapInnerBorderUnknownColor = IM_COL32(170, 178, 190, 52);
constexpr ImU32 kMapOuterBorderRedColor = IM_COL32(250, 63, 64, 220);
constexpr ImU32 kMapOuterBorderBlueColor = IM_COL32(64, 170, 245, 220);
constexpr ImU32 kMapOuterBorderUnknownColor = IM_COL32(170, 178, 190, 220);
constexpr ImU32 kMapMarkerBorderColor = IM_COL32(255, 176, 64, 235);

constexpr ImU32 kUnitCoreColor = IM_COL32(26, 28, 34, 255);
constexpr ImU32 kUnitTextColor = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kHighlightColor = IM_COL32(255, 215, 0, 255);
constexpr ImU32 kPathColor = IM_COL32(255, 153, 51, 220);

ImU32 GetMapInnerBorderColor(bool team_known, bool is_red_team) {
    if (!team_known) {
        return kMapInnerBorderUnknownColor;
    }
    return is_red_team ? kMapInnerBorderRedColor : kMapInnerBorderBlueColor;
}

ImU32 GetMapOuterBorderColor(bool team_known, bool is_red_team) {
    if (!team_known) {
        return kMapOuterBorderUnknownColor;
    }
    return is_red_team ? kMapOuterBorderRedColor : kMapOuterBorderBlueColor;
}

std::pair<ImVec2, ImVec2> GetMiniMapUvRect(const ImVec2 &texture_size) {
    constexpr float kMiniMapTextureLeft = 45.0f;
    constexpr float kMiniMapTextureTop = 31.0f;
    constexpr float kMiniMapTextureRight = 1226.0f;
    constexpr float kMiniMapTextureBottom = 683.0f;

    const float width = std::max(texture_size.x, 1.0f);
    const float height = std::max(texture_size.y, 1.0f);
    const ImVec2 uv_min(std::clamp(kMiniMapTextureLeft / width, 0.0f, 1.0f),
                        std::clamp(kMiniMapTextureTop / height, 0.0f, 1.0f));
    const ImVec2 uv_max(std::clamp(kMiniMapTextureRight / width, 0.0f, 1.0f),
                        std::clamp(kMiniMapTextureBottom / height, 0.0f, 1.0f));
    return {uv_min, uv_max};
}

}// namespace

void ShowMiniMap(const State &state,
                 RoboGo::io::MqttClient &mqtt_client,
                 const ImTextureRef mini_map_texture_ref,
                 const ImVec2 &mini_map_texture_size) {
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    if (!ImGui::Begin("MiniMap", nullptr, window_flags)) {
        ImGui::PopStyleVar();
        ImGui::End();
        return;
    }

    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    ImDrawList *foreground_draw_list = ImGui::GetForegroundDrawList();
    ImVec2 window_pos = ImGui::GetWindowPos();
    ImVec2 window_size = ImGui::GetWindowSize();
    ImVec2 map_min = window_pos;
    ImVec2 map_max(window_pos.x + window_size.x, window_pos.y + window_size.y);
    const float scale = ImGui::GetScale();
    const bool marker_mode_active = is_marker_mode();
    // 判断己方阵营：通过 robot_static_status 的 robot_id 判断（1-99 为红方，100+ 为蓝方）
    bool is_red_team = false;
    bool team_known = false;

    if (state.robot_static_status.has_value() && state.robot_static_status->has_robot_id()) {
        is_red_team = state.robot_static_status->robot_id() < 100;
        team_known = true;
    }

    // 添加不可见按钮使小地图可交互
    ImGui::PushID("MiniMapInteractive");
    ImGui::InvisibleButton("minimap_area", window_size);
    ImGui::PopID();

    // 绘制地图背景
    float outer_border_gap = 3.0f * scale;
    float outer_border_thickness = 3.0f * scale;
    ImVec2 outer_border_min(map_min.x - outer_border_gap - outer_border_thickness * 0.5f,
                            map_min.y - outer_border_gap - outer_border_thickness * 0.5f);
    ImVec2 outer_border_max(map_max.x + outer_border_gap + outer_border_thickness * 0.5f,
                            map_max.y + outer_border_gap + outer_border_thickness * 0.5f);

    draw_list->AddRectFilled(map_min, map_max, kMapBackgroundColor);
    if (mini_map_texture_ref.GetTexID() != ImTextureID_Invalid && mini_map_texture_size.x > 0.0f && mini_map_texture_size.y > 0.0f) {
        const auto [uv_min, uv_max] = GetMiniMapUvRect(mini_map_texture_size);
        draw_list->AddImage(mini_map_texture_ref, map_min, map_max, uv_min, uv_max);
        draw_list->AddRectFilled(map_min, map_max, kMapOverlayColor);
    }
    draw_list->AddRect(map_min, map_max, GetMapInnerBorderColor(team_known, is_red_team));
    // 外边框绘制在地图内容区域之外，避免改变地图坐标映射和点击标记范围。
    foreground_draw_list->AddRect(outer_border_min,
                                  outer_border_max,
                                  marker_mode_active ? kMapMarkerBorderColor : GetMapOuterBorderColor(team_known, is_red_team),
                                  0.0f,
                                  outer_border_thickness,
                                  0);

    // 绘制单位位置
    float map_content_width = window_size.x;
    float map_content_height = window_size.y;
    const char *text_begin, *text_end;

    // 只有确定己方阵营后才显示机器人位置
    if (team_known && state.radar_info_to_client.has_value()) {
        const auto &radar_info = *state.radar_info_to_client;

        // robots_info 按顺序：对方 1,2,3,4,6,7 号 (索引 0-5)，己方 1,2,3,4,6,7 号 (索引 6-11)
        // 如果己方是红方，则对方是蓝方(100+)，如果己方是蓝方，则对方是红方(1-99)
        const std::array<uint32_t, 12> robot_ids = is_red_team
            ? std::array<uint32_t, 12>{101, 102, 103, 104, 106, 107, 1, 2, 3, 4, 6, 7}  // 红方视角：对方蓝方在前，己方红方在后
            : std::array<uint32_t, 12>{1, 2, 3, 4, 6, 7, 101, 102, 103, 104, 106, 107}; // 蓝方视角：对方红方在前，己方蓝方在后

        for (int i = 0; i < radar_info.robots_info_size(); ++i) {
            const auto &robot_info = radar_info.robots_info(i);
            uint32_t robot_id = robot_ids[i];

            // 跳过未定位的机器人
            if (robot_info.target_pos_x() == 0 && robot_info.target_pos_y() == 0) {
                continue;
            }

            float screen_x = map_min.x + static_cast<float>(robot_info.target_pos_x()) / kMapWidth * map_content_width;
            float screen_y = map_min.y + static_cast<float>(robot_info.target_pos_y()) / kMapHeight * map_content_height;

            ImU32 unit_color = robot_id < 100 ? kTeamRedColor : kTeamBlueColor;
            ImU32 unit_fill_color = robot_id < 100 ? kTeamRedFillColor : kTeamBlueFillColor;
            float unit_radius = 10.0f * scale;
            ImVec2 unit_center(screen_x, screen_y);

            // 绘制单位图标
            draw_list->AddCircleFilled(unit_center, unit_radius, unit_fill_color);
            draw_list->AddCircleFilled(unit_center, unit_radius - 2.0f * scale, kUnitCoreColor);

            // 特殊标识（高亮）显示
            if (robot_info.is_high_light() > 0) {
                draw_list->AddCircle(unit_center, unit_radius + 3.0f * scale, kHighlightColor, 0, 2.0f * scale);
            }
            draw_list->AddCircle(unit_center, unit_radius, unit_color, 0, 2.0f * scale);

            // 绘制编号
            ImFormatStringToTempBuffer(&text_begin, &text_end, "%d", robot_id < 100 ? robot_id : robot_id - 100);
            ImGui::PushFont(ImGui::GetFont(), 12);
            ImVec2 text_size = ImGui::CalcTextSize(text_begin, text_end);
            draw_list->AddText(ImVec2(screen_x - text_size.x / 2, screen_y - text_size.y / 2), kUnitTextColor, text_begin, text_end);
            ImGui::PopFont();
        }
    }

    // 绘制哨兵路径
    if (state.robot_path_plan_info.has_value()) {
        auto current_x = static_cast<float>(state.robot_path_plan_info->start_pos_x());
        auto current_y = static_cast<float>(state.robot_path_plan_info->start_pos_y());
        float next_x;
        float next_y;
        int point_count = std::min(state.robot_path_plan_info->offset_x_size(),
                                   state.robot_path_plan_info->offset_y_size());
        for (int i = 0; i < point_count; i++) {
            next_x = current_x + static_cast<float>(state.robot_path_plan_info->offset_x(i));
            next_y = current_y + static_cast<float>(state.robot_path_plan_info->offset_y(i));

            float x1 = map_min.x + current_x / kMapWidth * map_content_width;
            float y1 = map_min.y + current_y / kMapHeight * map_content_height;
            float x2 = map_min.x + next_x / kMapWidth * map_content_width;
            float y2 = map_min.y + next_y / kMapHeight * map_content_height;

            draw_list->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), kPathColor, 2.0f * scale);

            current_x = next_x;
            current_y = next_y;
        }
    }

    // 处理标记模式下的点击事件
    if (is_marker_mode() && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImGuiIO &io = ImGui::GetIO();
        ImVec2 mouse_pos = io.MousePos;

        // 将屏幕坐标转换为地图坐标
        float map_x = (mouse_pos.x - map_min.x) / map_content_width * kMapWidth;
        float map_y = (mouse_pos.y - map_min.y) / map_content_height * kMapHeight;

        send_map_marker(mqtt_client, map_x, map_y);
    }

    ImGui::End();
    ImGui::PopStyleVar();
}
