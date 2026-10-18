#include "crosshair.h"

void ShowCrosshair(const State &state) {
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("CrosshairWindow", nullptr, window_flags)) {
        ImGui::End();
        return;
    }

    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    ImVec2 window_pos = ImGui::GetWindowPos();
    ImVec2 window_size = ImGui::GetWindowSize();
    ImVec2 center = ImVec2(window_pos.x + window_size.x / 2, window_pos.y + window_size.y / 2);

    // 绘制十字准星
    float crosshair_size = 10.0f;
    float crosshair_thickness = 1.0f;
    ImU32 crosshair_color = IM_COL32(255, 255, 255, 255);

    // 水平线
    draw_list->AddLine(
            ImVec2(center.x - crosshair_size, center.y),
            ImVec2(center.x + crosshair_size, center.y),
            crosshair_color, crosshair_thickness);

    // 垂直线
    draw_list->AddLine(
            ImVec2(center.x, center.y - crosshair_size),
            ImVec2(center.x, center.y + crosshair_size),
            crosshair_color, crosshair_thickness);

    // 绘制热量环
    float ring_radius = 40.0f;
    float ring_thickness = 4.0f;
    float heat_percentage;
    if (state.robot_dynamic_status.has_value() && state.robot_static_status.has_value() && state.robot_static_status->max_heat() != 0) {
        heat_percentage = state.robot_dynamic_status->current_heat() / static_cast<float>(state.robot_static_status->max_heat());
    } else {
        heat_percentage = 0.0f;
    }

    if (heat_percentage > 0.01f) {
        // 根据热量比例计算颜色（绿色 -> 黄色 -> 红色）
        ImU32 heat_color;
        if (heat_percentage < 0.5f) {
            // 绿色到黄色
            float ratio = heat_percentage * 2.0f;
            int r = static_cast<int>(255 * ratio);
            int g = 255;
            int b = 0;
            heat_color = IM_COL32(r, g, b, 255);
        } else {
            // 黄色到红色
            float ratio = (heat_percentage - 0.5f) * 2.0f;
            int r = 255;
            int g = static_cast<int>(255 * (1.0f - ratio));
            int b = 0;
            heat_color = IM_COL32(r, g, b, 255);
        }

        // 绘制热量环
        float end_angle = heat_percentage * 2.0f * 3.14159f;
        draw_list->PathArcTo(center, ring_radius, 0.0f, end_angle, 32);
        draw_list->PathStroke(heat_color, ring_thickness, ImDrawFlags_None);
    }

    ImGui::End();
}
