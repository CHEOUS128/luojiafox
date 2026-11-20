#include "teammate_panel.h"
#include "hud_text.h"

namespace {

bool HasImage(const TeammatePeerView &peer) {
    return !peer.paused && peer.display_ready && peer.texture_ref.GetTexID() != ImTextureID_Invalid
           && peer.texture_size.x > 0.f && peer.texture_size.y > 0.f;
}

const char *Status(const TeammatePeerView &peer) {
    if (peer.paused) return "测试信号已断开";
    if (HasImage(peer)) return peer.live ? "在线" : "画面暂停";
    return peer.peer_connected || peer.peer_served ? "等待图传" : "等待连接";
}

} // namespace

// 标题始终在视频上方。收不到画面时仍在原槽位显示岗位和原因。
bool DrawTeammateTile(ImDrawList *draw, const TeammatePeerView &peer, const ImRect &rect,
              float zoom, bool large) {
    if (rect.GetWidth() <= 0.f || rect.GetHeight() <= 0.f) return false;
    draw->PushClipRect(rect.Min, rect.Max, true);
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const char *status = Status(peer);
    const ImU32 status_color = HasImage(peer) && peer.live ? IM_COL32(144, 225, 185, 255)
                               : HasImage(peer) ? IM_COL32(250, 202, 122, 255) : hud::muted;
    hud::Outline(draw, rect.Min, hud::white, peer.label.c_str());
    const float status_width = ImGui::CalcTextSize(status).x;
    if (rect.GetWidth() >= ImGui::CalcTextSize(peer.label.c_str()).x + status_width + 8.f) {
        hud::Outline(draw, {rect.Max.x - status_width, rect.Min.y}, status_color, status);
    }
    const ImRect picture({rect.Min.x, std::min(rect.Max.y, rect.Min.y + line)}, rect.Max);
    const bool draw_image = picture.GetHeight() > 0.f && HasImage(peer);
    if (draw_image) {
        const float fit = std::min(picture.GetWidth() / peer.texture_size.x,
                                   picture.GetHeight() / peer.texture_size.y);
        const ImVec2 center = picture.GetCenter();
        const ImVec2 half(peer.texture_size.x * fit * .5f, peer.texture_size.y * fit * .5f);
        // 滚轮只改变取样范围；画面边框保持原来的大小和比例。
        const float extent = .5f / std::clamp(zoom, 1.f, 4.f);
        const float a = .5f - extent;
        const float b = .5f + extent;
        const ImVec2 uv0 = peer.frame_inverted ? ImVec2(b, b) : ImVec2(a, a);
        const ImVec2 uv1 = peer.frame_inverted ? ImVec2(a, a) : ImVec2(b, b);
        draw->AddImage(peer.texture_ref, {center.x - half.x, center.y - half.y},
                       {center.x + half.x, center.y + half.y}, uv0, uv1);
    } else if (large) {
        const char *waiting = peer.paused ? "等待恢复测试信号" :
                              peer.peer_connected || peer.peer_served ? "已连接，等待队友画面" : "等待队友连接";
        const ImVec2 text = ImGui::CalcTextSize(waiting);
        const ImVec2 c = picture.GetCenter();
        hud::Outline(draw, {c.x - text.x * .5f, c.y - text.y * .5f}, hud::muted, waiting);
    }
    draw->PopClipRect();
    return draw_image;
}

float ShowTeammateSidebar(const TeammateViewState &view, ImVec2 pos, float width, float max_height) {
    const int count = std::clamp(view.count, 0, static_cast<int>(view.peers.size()));
    if (!count || width <= 0.f || max_height <= 0.f) return 0.f;
    const float pad = std::max(4.f, ImGui::GetStyle().WindowPadding.x);
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const float header = line + 2.f * pad;
    const float inner_width = std::max(1.f, width - 2.f * pad);
    const float gap = std::max(3.f, line * .2f);
    const float cell = std::min(line + inner_width * 9.f / 16.f,
                                std::max(0.f, (max_height - header - (count - 1) * gap) / count));
    ImDrawList *draw = ImGui::GetBackgroundDrawList();
    draw->PushClipRect(pos, {pos.x + width, pos.y + max_height}, true);
    const ImVec2 at(pos.x + pad, pos.y + pad);
    hud::Outline(draw, at, hud::white, "队友视角");
    const char *keys = view.collapsed ? "K 展开 · L 查看" : "K 收起 · L 查看";
    const float keys_width = ImGui::CalcTextSize(keys).x;
    if (ImGui::CalcTextSize("队友视角").x + keys_width + pad < inner_width)
        hud::Outline(draw, {pos.x + width - pad - keys_width, at.y}, hud::muted, keys);
    float y = pos.y + header;
    if (!view.collapsed) {
        for (int i = 0; i < count; ++i) {
            DrawTeammateTile(draw, view.peers[i], {{at.x, y}, {at.x + inner_width, y + cell}}, 1.f, false);
            y += cell + (i + 1 < count ? gap : 0.f);
        }
    }
    draw->PopClipRect();
    return std::min(max_height, y - pos.y);
}

int ShowTeammateOverview(const TeammateViewState &view, bool menu_open, bool local_test, unsigned& drawn_mask) {
    const int count = std::clamp(view.count, 0, static_cast<int>(view.peers.size()));
    if (!count) return -1;
    const bool focused = view.focused >= 0 && view.focused < count;
    const int columns = focused ? 1 : count;
    const auto *vp = ImGui::GetMainViewport();
    const ImVec2 end(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
    auto *draw = ImGui::GetBackgroundDrawList();
    draw->AddRectFilled(vp->Pos, end, IM_COL32(14, 18, 22, 255));

    const float current_font = ImGui::GetFontSize();
    const float target_font = std::clamp(2.f * std::floor(std::min(vp->Size.x / 88.f, vp->Size.y / 48.f) / 2.f),
                                         current_font, std::max(current_font, 38.f));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * target_font / current_font);
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const float pad = std::max(6.f, line * .6f);
    const ImVec2 top(vp->Pos.x + pad, vp->Pos.y + pad);
    const char *title = local_test ? "队友视角 · 本地测试" : "队友视角";
    const char *hint = columns == 2 ? "单击画面放大 · L 回本车"
                       : count == 2 ? "单击回到双画面 · 滚轮缩放 · L 回本车" : "滚轮缩放 · L 回本车";
    hud::Outline(draw, top, hud::white, title);
    const float hint_x = end.x - pad - ImGui::CalcTextSize(hint).x;
    if (hint_x > top.x + ImGui::CalcTextSize(title).x + pad)
        hud::Outline(draw, {hint_x, top.y}, hud::muted, hint);

    // 画面画在背景层，菜单可以盖在上面。点击只在总览独立窗口接收。
    const bool allow_click = count == 2 && !menu_open;
    if (allow_click) {
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##TeammateOverviewInput", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                     | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav
                     | ImGuiWindowFlags_NoBringToFrontOnFocus);
    }
    int clicked = -1;
    const float cell_width = std::max(1.f, (vp->Size.x - (columns + 1) * pad) / columns);
    for (int column = 0; column < columns; ++column) {
        const int index = focused ? view.focused : column;
        const float x = top.x + column * (cell_width + pad);
        const ImRect rect({x, top.y + line + pad}, {x + cell_width, end.y - pad});
        if (DrawTeammateTile(draw, view.peers[index], rect, columns == 1 ? view.zoom : 1.f, true))
            drawn_mask |= 1u << index;
        if (allow_click && rect.GetHeight() > 0.f) {
            ImGui::SetCursorScreenPos(rect.Min);
            ImGui::PushID(index);
            if (ImGui::InvisibleButton("peer", rect.GetSize())) clicked = index;
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::PopID();
        }
    }
    if (allow_click) {
        ImGui::End();
        ImGui::PopStyleVar();
    }
    ImGui::PopFont();
    return clicked;
}
