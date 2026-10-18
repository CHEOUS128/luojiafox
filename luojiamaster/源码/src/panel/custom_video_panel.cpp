#include "custom_video_panel.h"

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

void ShowCenteredText(const char *message) {
    const ImVec2 child_avail = ImGui::GetContentRegionAvail();
    const float wrap_width = ImMax(0.0f, child_avail.x - ImGui::GetStyle().WindowPadding.x * 2.0f);
    const ImVec2 text_size = ImGui::CalcTextSize(message, nullptr, false, wrap_width);
    ImVec2 cursor = ImGui::GetCursorPos();
    cursor.x += ImMax(0.0f, (child_avail.x - text_size.x) * 0.5f);
    cursor.y += ImMax(0.0f, (child_avail.y - text_size.y) * 0.5f);
    ImGui::SetCursorPos(cursor);
    ImGui::PushTextWrapPos(cursor.x + wrap_width);
    ImGui::TextDisabled("%s", message);
    ImGui::PopTextWrapPos();
}

}// namespace

bool ShowCustomVideoPanel(ImTextureRef video_texture_ref,
                          const ImVec2 &video_size,
                          const char *panel_title,
                          const bool video_display_ready,
                          const std::string_view unavailable_message,
                          const bool udp_video_inverted) {
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("CustomVideoPanel", nullptr, window_flags)) {
        ImGui::End();
        return false;
    }

    bool toggle_udp_video_inversion_requested = false;
    ImGui::Text("%s", panel_title);
    ImGui::SameLine();
    ImGui::TextDisabled("[H 切换]");
    const char *invert_button_label = udp_video_inverted
                                              ? "UDP反转: 开 [I]##UdpVideoInvert"
                                              : "UDP反转: 关 [I]##UdpVideoInvert";
    const char *visible_label_end = std::strstr(invert_button_label, "##");
    const ImVec2 visible_label_size = ImGui::CalcTextSize(invert_button_label, visible_label_end);
    const float button_width = visible_label_size.x + ImGui::GetStyle().FramePadding.x * 2.0f;
    if (ImGui::GetContentRegionAvail().x >= button_width + ImGui::GetStyle().ItemSpacing.x) {
        ImGui::SameLine();
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
        const ImVec2 uv0 = udp_video_inverted ? ImVec2(1.0f, 1.0f) : ImVec2(0.0f, 0.0f);
        const ImVec2 uv1 = udp_video_inverted ? ImVec2(0.0f, 0.0f) : ImVec2(1.0f, 1.0f);
        ImGui::Image(video_texture_ref, image_size, uv0, uv1);
    } else {
        const std::string message(unavailable_message);
        ShowCenteredText(message.c_str());
    }
    ImGui::EndChild();

    ImGui::End();
    return toggle_udp_video_inversion_requested;
}
