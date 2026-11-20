#pragma once
#include "panel/replay_card_layout.h"
#include "panel/teammate_panel.h"

namespace luojiamaster::replay_cards {

struct RecordingSlot {
    const char *label = "";
    const TeammatePeerView *picture = nullptr;
    int source = -1;
};

// Used by the two local replay screens. 'source' is the decoder's slot;
// the array order is the card's stable position within the selected seat.
inline void DrawRecordingCards(Layout &layout, int focused, std::array<float,3> &zoom,
        const RecordingSlot *slots, int count, const RoboGo::arena::ReplayCanvas &canvas,
        bool show_markers, bool interactive, int previous_hit, int &clicked,
        unsigned &drawn_mask, ImVec2 *thumbnail_pixels = nullptr) {
    namespace cards = luojiamaster::replay_cards;
    const auto &io = ImGui::GetIO();
    const bool accepts_mouse = interactive && !canvas.map_dragging && ImGui::IsWindowHovered()
                              && !io.AppFocusLost;
    if (!accepts_mouse) layout.pressed = -1;
    const int held = accepts_mouse ? (layout.pressed >= 0 ? layout.pressed : previous_hit) : -1;
    cards::Place(layout, canvas, focused, held, ImGui::GetFontSize(), count);
    const int hit = accepts_mouse ? layout.Hit(io.MousePos, focused, count) : -1;
    if (hit >= 0) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        zoom[hit] = std::clamp(zoom[hit] + io.MouseWheel * .25f, 1.f, 4.f);
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) layout.pressed = hit;
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (layout.pressed >= 0 && layout.pressed == hit &&
            io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] <= io.MouseDragThreshold * io.MouseDragThreshold)
            clicked = hit;
        layout.pressed = -1;
    }
    auto *draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(canvas.lo, canvas.hi, true);
    const ImU32 border = IM_COL32(114, 146, 151, 235);
    const ImU32 accent = IM_COL32(124, 226, 209, 255);
    for (int slot = 0; slot < count; ++slot) {
        const auto &pin = canvas.pins[slot];
        if (!pin.has_position || !pin.inside) continue;
        const auto end = cards::Closest(layout.boxes[slot],pin.screen);
        draw->AddLine(pin.screen,end,IM_COL32(10,19,23,215),3.f);
        draw->AddLine(pin.screen,end,slot == focused ? accent : IM_COL32(212,227,223,225),1.3f);
    }
    const float inset = std::max(3.f, ImGui::GetFontSize() * .24f);
    const float rounding = std::max(3.f, inset);
    // Paint focused last, matching Hit's topmost order even in a tiny window.
    for (int i = 0; i <= count; ++i) {
        const int slot = i == count ? focused : i;
        if (slot < 0 || (i < count && slot == focused)) continue;
        const ImRect &box = layout.boxes[slot];
        draw->AddRectFilled(box.Min,box.Max,IM_COL32(19,29,33,246),rounding);
        draw->AddRect(box.Min,box.Max,slot == focused || hit == slot ? accent : border,
                      rounding,slot == focused ? 2.f : 1.f);
        ImRect content = box;
        content.Expand(-inset);
        TeammatePeerView waiting{};
        waiting.label = slots[slot].label;
        const auto &feed = slots[slot].picture ? *slots[slot].picture : waiting;
        if (DrawTeammateTile(draw, feed, content, zoom[slot], true)) drawn_mask |= 1u << slots[slot].source;
        if (thumbnail_pixels && slot != focused) {
            const auto scale = io.DisplayFramebufferScale;
            *thumbnail_pixels = {content.GetWidth() * scale.x,
                (content.GetHeight() - ImGui::GetTextLineHeightWithSpacing()) * scale.y};
        }
        const auto &pin = canvas.pins[slot];
        const char *hint = !show_markers ? "棋子已隐藏" : !pin.has_position ? "位置待更新" :
                           !pin.inside ? "在画面外" : pin.age_seconds >= 3 ? "位置未更新" : nullptr;
        if (hint && content.GetWidth() > ImGui::CalcTextSize(hint).x + inset) {
            const ImVec2 text = ImGui::CalcTextSize(hint);
            const ImVec2 badge{content.Min.x, content.Max.y-text.y-inset};
            draw->AddRectFilled(badge, {badge.x+text.x+inset*2,content.Max.y}, IM_COL32(16,28,33,233),2.f);
            draw->AddText({badge.x+inset,badge.y+inset*.5f},IM_COL32(239,206,156,255),hint);
        }
    }
    draw->PopClipRect();
}
} // namespace luojiamaster::replay_cards
