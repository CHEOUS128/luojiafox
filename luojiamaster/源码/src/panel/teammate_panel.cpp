#include "teammate_panel.h"

#include <cstdio>

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

// 队友画面是否倒置：
//   官方 UDP 图传本身是 180° 倒的，我们平时靠 GPU 的 UV 翻转显示。
//   队友那一路是把「原始字节」原样转过来的，所以对方握手告诉我们的
//   「他那边画面倒不倒」就是这段字节的真实朝向，直接用，不要动。
bool PeerFrameInverted(const TeammateViewState &view) {
    return view.frame_inverted;
}

const char *LinkStatusText(const TeammateViewState &view) {
    const bool outgoing = view.peer_connected;
    const bool incoming = view.peer_served;
    if (outgoing && incoming) {
        return "双向已连";
    }
    if (outgoing) {
        return "已连队友";
    }
    if (incoming) {
        return "队友已连入";
    }
    return "未连接";
}

ImU32 TitleColor(const TeammateViewState &view) {
    if (view.live) {
        return IM_COL32(120, 220, 140, 255);
    }
    if (view.display_ready) {
        return IM_COL32(240, 200, 90, 255);
    }
    return IM_COL32(230, 100, 100, 255);
}

}// namespace

float TeammateCollapsedHeight() {
    return ImGui::GetFontSize() + ImGui::GetStyle().WindowPadding.y * 2.0f + 4.0f;
}

bool ShowTeammateViewport(const TeammateViewState &view,
                          const ImVec2 &pos,
                          const ImVec2 &size,
                          const bool collapsed) {
    if (!view.enabled) {
        return false;
    }

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration
                             | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("TeammateViewPanel", nullptr, flags)) {
        ImGui::End();
        return false;
    }

    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TitleColor(view)), "队友视角");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("K 折叠/展开\nL 占主画面（再按退出，滚轮 1x~4x 放大）");
    }
    ImGui::SameLine(0.0f, 6.0f);
    // 角色标签：点一下在 A/B 之间切换（换对方的 IP），选择会写回 exe 同目录的 ini。
    // 只改队友链路的地址，不碰本机官方链路；切换后链路按新地址重连。
    // 用 ImGuiKey_MouseLeft 只认鼠标左键，不会被发给机器人的键影响。
    const bool role_b = (view.role == 'B');
    ImGui::PushStyleColor(ImGuiCol_Text, role_b ? IM_COL32(150, 200, 255, 255)
                                               : IM_COL32(255, 190, 120, 255));
    const char *role_label = view.role_from_default ? (role_b ? "[默认B]" : "[默认A]")
                                                    : (role_b ? "[B]" : "[A]");
    ImGui::TextUnformatted(role_label);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImGui::End();
        return true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("队友角色：点一下切到 %s 机\nA 机：找 10.77.0.2:47700\nB 机：找 10.77.0.1:47700",
                          role_b ? "A" : "B");
    }
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextDisabled("%s", LinkStatusText(view));

    if (collapsed) {
        ImGui::End();
        return false;
    }

    // 小窗画面：显示最后一帧，超过 5 秒没新帧才换占位。
    ImGui::BeginChild("TeammateVideoChild", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (view.display_ready && view.texture_ref.GetTexID() != ImTextureID_Invalid
        && view.texture_size.x > 0.0f && view.texture_size.y > 0.0f) {
        const ImVec2 child_avail = ImGui::GetContentRegionAvail();
        const ImVec2 image_size = FitImageSize(child_avail, view.texture_size);
        ImVec2 cursor = ImGui::GetCursorPos();
        cursor.x += (child_avail.x - image_size.x) * 0.5f;
        cursor.y += (child_avail.y - image_size.y) * 0.5f;
        ImGui::SetCursorPos(cursor);
        const bool inverted = PeerFrameInverted(view);
        const ImVec2 uv0 = inverted ? ImVec2(1.0f, 1.0f) : ImVec2(0.0f, 0.0f);
        const ImVec2 uv1 = inverted ? ImVec2(0.0f, 0.0f) : ImVec2(1.0f, 1.0f);
        ImGui::Image(view.texture_ref, image_size, uv0, uv1);
    } else {
        // 默认 A 机、对方还没上线时，把该填的 IP 直接写出来，省得再翻文档。
        const std::string message = view.peer_connected || view.peer_served
                                            ? "队友画面等待中..."
                                            : std::string("未连接队友（本机 ") + view.role
                                                      + " 机，找 " + (view.role == 'B' ? "10.77.0.1" : "10.77.0.2")
                                                      + "）；连不上先查队友网线和对方客户端";
        ShowCenteredText(message.c_str());
    }
    ImGui::EndChild();
    ImGui::End();
    return false;
}

void ShowTeammateFullscreen(const TeammateViewState &view) {
    if (!view.enabled || !view.take_main_screen) {
        return;
    }

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    // 直接画在背景层（和本车主画面同一层），HUD 面板照常显示在上面；
    // 状态条画在前景层，保证不会被 HUD 盖住。
    ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
    ImDrawList *foreground = ImGui::GetForegroundDrawList();
    const bool has_image = view.display_ready && view.texture_ref.GetTexID() != ImTextureID_Invalid
                           && view.texture_size.x > 0.0f && view.texture_size.y > 0.0f;

    if (has_image) {
        // 1x 时整幅等比铺满；放大时保持中心不动，只缩小 UV 窗口。
        const ImVec2 fitted = FitImageSize(viewport->Size, view.texture_size);
        const float zoom = ImClamp(view.zoom, 1.0f, 4.0f);
        const float half_u = 0.5f / zoom;
        const float half_v = 0.5f / zoom;
        float u0 = 0.5f - half_u;
        float v0 = 0.5f - half_v;
        float u1 = 0.5f + half_u;
        float v1 = 0.5f + half_v;
        if (PeerFrameInverted(view)) {
            std::swap(u0, u1);
            std::swap(v0, v1);
        }
        const float draw_width = fitted.x / zoom;
        const float draw_height = fitted.y / zoom;
        const ImVec2 image_min((viewport->Size.x - draw_width) * 0.5f,
                               (viewport->Size.y - draw_height) * 0.5f);
        const ImVec2 image_max(image_min.x + draw_width, image_min.y + draw_height);
        draw_list->AddImage(view.texture_ref, image_min, image_max, ImVec2(u0, v0), ImVec2(u1, v1));

        // 左上角状态条：缩放倍数 + 已解帧数 + 朝向
        char overlay[128] = {};
        snprintf(overlay,
                 sizeof(overlay),
                 "队友视角 %.2fx  已解 %llu 帧  %s",
                 static_cast<double>(zoom),
                 static_cast<unsigned long long>(view.frames_decoded),
                 PeerFrameInverted(view) ? "翻转" : "正置");
        const float text_height = ImGui::GetFontSize();
        const ImVec2 overlay_min(4.0f, 4.0f);
        const ImVec2 overlay_text_size = ImGui::CalcTextSize(overlay);
        foreground->AddRectFilled(overlay_min,
                                  ImVec2(overlay_min.x + overlay_text_size.x + 12.0f,
                                         overlay_min.y + text_height + 8.0f),
                                  IM_COL32(0, 0, 0, 170));
        foreground->AddText(ImVec2(overlay_min.x + 6.0f, overlay_min.y + 4.0f),
                            IM_COL32(255, 255, 255, 235),
                            overlay);
    } else {
        const char *message = view.peer_connected || view.peer_served
                                      ? "队友画面等待中... 按 L 退出"
                                      : "未连接队友... 按 L 退出";
        const ImVec2 text_size = ImGui::CalcTextSize(message);
        foreground->AddRectFilled(ImVec2(0.0f, 0.0f), viewport->Size, IM_COL32(0, 0, 0, 235));
        foreground->AddText(ImVec2((viewport->Size.x - text_size.x) * 0.5f,
                                   (viewport->Size.y - text_size.y) * 0.5f),
                            IM_COL32(220, 220, 220, 255),
                            message);
    }
}
