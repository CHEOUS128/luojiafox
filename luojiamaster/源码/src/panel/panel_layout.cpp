#include "panel_layout.h"

namespace RoboGo::panel {

ScopedStyle::ScopedStyle(ScopedStyle &&other) noexcept
    : color_count_(other.color_count_), var_count_(other.var_count_) {
    other.color_count_ = 0;
    other.var_count_ = 0;
}

ScopedStyle &ScopedStyle::operator=(ScopedStyle &&other) noexcept {
    if (this != &other) {
        Reset();
        color_count_ = other.color_count_;
        var_count_ = other.var_count_;
        other.color_count_ = 0;
        other.var_count_ = 0;
    }
    return *this;
}

ScopedStyle::~ScopedStyle() {
    Reset();
}

void ScopedStyle::PushColor(ImGuiCol index, const ImVec4 &color) {
    ImGui::PushStyleColor(index, color);
    color_count_++;
}

void ScopedStyle::PushVar(ImGuiStyleVar index, float value) {
    ImGui::PushStyleVar(index, value);
    var_count_++;
}

void ScopedStyle::PushVar(ImGuiStyleVar index, const ImVec2 &value) {
    ImGui::PushStyleVar(index, value);
    var_count_++;
}

void ScopedStyle::Reset() {
    if (var_count_ > 0) {
        ImGui::PopStyleVar(var_count_);
        var_count_ = 0;
    }
    if (color_count_ > 0) {
        ImGui::PopStyleColor(color_count_);
        color_count_ = 0;
    }
}

ScopedChildWindow::ScopedChildWindow(const char *name, const ImVec2 &size, bool border, ImGuiWindowFlags flags) {
    ImGui::BeginChild(name, size, border, flags);
}

ScopedChildWindow::~ScopedChildWindow() {
    ImGui::EndChild();
}

ScopedTitledPanel::ScopedTitledPanel(const char *panel_id, float panel_width, const char *title, const ImVec4 &color)
    : child_(panel_id, ImVec2(panel_width, 0), false) {
    DrawPanelTitle(title, color);
}

ScopedStyle MakeTextColorStyle(const ImVec4 &color) {
    ScopedStyle style;
    style.PushColor(ImGuiCol_Text, color);
    return style;
}

ScopedStyle MakeInputStyle() {
    ScopedStyle style;
    style.PushColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.2f, 0.25f, 0.8f));
    style.PushColor(ImGuiCol_FrameBgHovered, ImVec4(0.2f, 0.25f, 0.3f, 0.9f));
    style.PushColor(ImGuiCol_FrameBgActive, ImVec4(0.25f, 0.3f, 0.35f, 1.0f));
    style.PushVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 6.0f));
    style.PushVar(ImGuiStyleVar_FrameRounding, 4.0f);
    return style;
}

ScopedStyle MakeButtonStyle(const ButtonStyle &button_style) {
    ScopedStyle style;
    style.PushColor(ImGuiCol_Button, button_style.button_color);
    style.PushColor(ImGuiCol_ButtonHovered, button_style.hovered_color);
    style.PushColor(ImGuiCol_ButtonActive, button_style.active_color);
    style.PushVar(ImGuiStyleVar_FramePadding, button_style.padding);
    style.PushVar(ImGuiStyleVar_FrameRounding, button_style.rounding);
    return style;
}

ScopedStyle MakeMouseSensitivitySliderStyle() {
    ScopedStyle style;
    style.PushColor(ImGuiCol_SliderGrab, ImVec4(1.0f, 0.8f, 0.3f, 0.8f));
    style.PushColor(ImGuiCol_SliderGrabActive, ImVec4(1.0f, 0.9f, 0.4f, 1.0f));
    style.PushVar(ImGuiStyleVar_GrabMinSize, 10.0f);
    style.PushVar(ImGuiStyleVar_GrabRounding, 4.0f);
    return style;
}

ScopedStyle MakeWindowStyle(const ImVec4 &background_color, float rounding, const ImVec2 &padding) {
    ScopedStyle style;
    style.PushColor(ImGuiCol_WindowBg, background_color);
    style.PushVar(ImGuiStyleVar_WindowRounding, rounding);
    style.PushVar(ImGuiStyleVar_WindowPadding, padding);
    return style;
}

void DrawPanelTitle(const char *title, const ImVec4 &color) {
    ScopedStyle title_style = MakeTextColorStyle(color);
    ImGui::Text("%s", title);
    ImGui::Separator();
    ImGui::Spacing();
}

float GetPanelWidth(int panel_count) {
    return panel_count > 0 ? ImGui::GetContentRegionAvail().x / static_cast<float>(panel_count) + static_cast<float>(panel_count - 1) * 5.0f : 0.0f;
}

void PlaceNextPanel(bool &is_first) {
    if (is_first) {
        is_first = false;
    } else {
        ImGui::SameLine();
    }
}

void ShowPanelRow(const char *name, float frame_row_count, std::initializer_list<PanelRowItem> panels) {
    int panel_count = 0;
    for (const auto &panel : panels) {
        if (panel.visible) {
            panel_count++;
        }
    }
    if (panel_count == 0) {
        return;
    }

    ScopedChildWindow row(name,
                          ImVec2(0, ImGui::GetFrameHeightWithSpacing() * frame_row_count + ImGui::GetStyle().SeparatorSize),
                          false,
                          ImGuiWindowFlags_NoScrollbar);
    float panel_width = GetPanelWidth(panel_count);
    bool is_first = true;
    for (const auto &panel : panels) {
        if (!panel.visible) {
            continue;
        }
        PlaceNextPanel(is_first);
        panel.render(panel_width);
    }
}

}// namespace RoboGo::panel
