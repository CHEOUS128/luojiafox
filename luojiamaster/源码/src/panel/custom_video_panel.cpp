#include "custom_video_panel.h"
#include "hud_text.h"

namespace {

ImVec2 FitImageSize(const ImVec2 &available_size, const ImVec2 &texture_size) {
    if (available_size.x <= 0.0f || available_size.y <= 0.0f || texture_size.x <= 0.0f || texture_size.y <= 0.0f) {
        return ImVec2(0.0f, 0.0f);
    }

    const float image_aspect = texture_size.x / texture_size.y;
    const float available_aspect = available_size.x / available_size.y;
    if (image_aspect > available_aspect) {
        return ImVec2(available_size.x, available_size.x / image_aspect);
    }
    return ImVec2(available_size.y * image_aspect, available_size.y);
}

void ShowCenteredText(const char *message, bool alliance) {
    const ImVec2 child_avail = ImGui::GetContentRegionAvail();
    const float wrap_width = ImMax(0.0f, child_avail.x - ImGui::GetStyle().WindowPadding.x * 2.0f);
    const ImVec2 text_size = ImGui::CalcTextSize(message, nullptr, false, wrap_width);
    ImVec2 cursor = ImGui::GetCursorPos();
    cursor.x += ImMax(0.0f, (child_avail.x - text_size.x) * 0.5f);
    cursor.y += ImMax(0.0f, (child_avail.y - text_size.y) * 0.5f);
    ImGui::SetCursorPos(cursor);
    ImGui::PushTextWrapPos(cursor.x + wrap_width);
    if (alliance) hud::Label(message, hud::muted, true);
    else ImGui::TextDisabled("%s", message);
    ImGui::PopTextWrapPos();
}

}// namespace

bool ShowCustomVideoPanel(ImTextureRef video_texture_ref,
                          const ImVec2 &video_size,
                          const char *panel_title,
                          const bool video_display_ready,
                          const std::string_view unavailable_message,
                          const bool udp_video_inverted, const bool secondary_is_udp,
                          bool *swap_requested) {
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("CustomVideoPanel", nullptr, window_flags)) {
        ImGui::End();
        return false;
    }

    bool toggle_udp_video_inversion_requested = false;
    const char *invert_button_label = nullptr;
    const auto &style = ImGui::GetStyle();
    if (swap_requested) {
        const float available = ImGui::GetContentRegionAvail().x;
        const float title_width = ImGui::CalcTextSize(panel_title).x;
        const float padding = 4.f * style.FramePadding.x;
        const float long_width = title_width + ImGui::CalcTextSize("切换 H").x +
                                 ImGui::CalcTextSize("翻转 I").x + padding + 2.f * style.ItemSpacing.x;
        const bool compact = long_width > available;
        const char *swap_label = compact ? "H" : "切换 H";
        const char *invert_label = compact ? "I" : "翻转 I";
        const float buttons_width = ImGui::CalcTextSize(swap_label).x + ImGui::CalcTextSize(invert_label).x +
                                    padding + style.ItemSpacing.x;
        hud::Label(panel_title);
        if (title_width + style.ItemSpacing.x + buttons_width <= available) ImGui::SameLine();
        *swap_requested = ImGui::SmallButton(swap_label);
        if (compact && ImGui::IsItemHovered()) ImGui::SetTooltip("H · 交换主副画面");
        if (buttons_width <= available) ImGui::SameLine();
        invert_button_label = compact ? "I##UdpVideoInvert" : "翻转 I##UdpVideoInvert";
    } else {
        ImGui::Text("%s", panel_title);
        ImGui::SameLine();
        ImGui::TextDisabled("[H 切换]");
        invert_button_label = udp_video_inverted ? "UDP反转: 开 [I]##UdpVideoInvert" : "UDP反转: 关 [I]##UdpVideoInvert";
        const float button_width = ImGui::CalcTextSize(invert_button_label, nullptr, true).x + 2.f * style.FramePadding.x;
        if (ImGui::GetContentRegionAvail().x >= button_width + style.ItemSpacing.x) ImGui::SameLine();
    }
    ImGui::PushStyleColor(ImGuiCol_Button,
                          udp_video_inverted ? ImVec4(0.12f, 0.45f, 0.26f, 0.82f)
                                             : ImVec4(0.16f, 0.24f, 0.31f, 0.82f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          udp_video_inverted ? ImVec4(0.16f, 0.58f, 0.34f, 0.95f)
                                             : ImVec4(0.20f, 0.36f, 0.44f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          udp_video_inverted ? ImVec4(0.20f, 0.70f, 0.42f, 1.00f)
                                             : ImVec4(0.25f, 0.48f, 0.58f, 1.00f));
    toggle_udp_video_inversion_requested = ImGui::SmallButton(invert_button_label);
    if (swap_requested && ImGui::IsItemHovered()) ImGui::SetTooltip("I · 将官方画面翻转 180°");
    ImGui::PopStyleColor(3);
    ImGui::BeginChild("CustomVehicleVideo", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (video_display_ready
        && video_texture_ref.GetTexID() != ImTextureID_Invalid
        && video_size.x > 0.0f
        && video_size.y > 0.0f) {
        const ImVec2 child_avail = ImGui::GetContentRegionAvail();
        const ImVec2 image_size = FitImageSize(child_avail, video_size);
        ImVec2 cursor = ImGui::GetCursorPos();
        cursor.x += (child_avail.x - image_size.x) * 0.5f;
        cursor.y += (child_avail.y - image_size.y) * 0.5f;
        ImGui::SetCursorPos(cursor);
        // 副画面同样用 GPU UV 翻转做 180°（与主画面保持一致）
        const bool invert_image = secondary_is_udp && udp_video_inverted;
        const ImVec2 uv0 = invert_image ? ImVec2(1.0f, 1.0f) : ImVec2(0.0f, 0.0f);
        const ImVec2 uv1 = invert_image ? ImVec2(0.0f, 0.0f) : ImVec2(1.0f, 1.0f);
        ImGui::Image(video_texture_ref, image_size, uv0, uv1);
    } else {
        const std::string message(unavailable_message);
        ShowCenteredText(message.c_str(), swap_requested != nullptr);
    }
    ImGui::EndChild();

    ImGui::End();
    return toggle_udp_video_inversion_requested;
}
