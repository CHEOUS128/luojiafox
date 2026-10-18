#pragma once

#include <pch.h>

namespace RoboGo::panel {

struct ButtonStyle {
    ImVec4 button_color;
    ImVec4 hovered_color;
    ImVec4 active_color;
    ImVec2 padding = ImVec2(12.0f, 8.0f);
    float rounding = 6.0f;
};

struct PanelRowItem {
    bool visible = false;
    std::function<void(float)> render;
};

class ScopedStyle {
public:
    ScopedStyle() = default;
    ScopedStyle(const ScopedStyle &) = delete;
    ScopedStyle &operator=(const ScopedStyle &) = delete;
    ScopedStyle(ScopedStyle &&other) noexcept;
    ScopedStyle &operator=(ScopedStyle &&other) noexcept;
    ~ScopedStyle();

    void PushColor(ImGuiCol index, const ImVec4 &color);
    void PushVar(ImGuiStyleVar index, float value);
    void PushVar(ImGuiStyleVar index, const ImVec2 &value);

private:
    void Reset();

    int color_count_ = 0;
    int var_count_ = 0;
};

class ScopedChildWindow {
public:
    ScopedChildWindow(const char *name, const ImVec2 &size, bool border = false, ImGuiWindowFlags flags = 0);
    ScopedChildWindow(const ScopedChildWindow &) = delete;
    ScopedChildWindow &operator=(const ScopedChildWindow &) = delete;
    ~ScopedChildWindow();
};

class ScopedTitledPanel {
public:
    ScopedTitledPanel(const char *panel_id, float panel_width, const char *title, const ImVec4 &color);

private:
    ScopedChildWindow child_;
};

ScopedStyle MakeTextColorStyle(const ImVec4 &color);
ScopedStyle MakeInputStyle();
ScopedStyle MakeButtonStyle(const ButtonStyle &button_style);
ScopedStyle MakeMouseSensitivitySliderStyle();
ScopedStyle MakeWindowStyle(const ImVec4 &background_color, float rounding, const ImVec2 &padding);

void DrawPanelTitle(const char *title, const ImVec4 &color);

void ShowPanelRow(const char *name, float frame_row_count, std::initializer_list<PanelRowItem> panels);

}// namespace RoboGo::panel
