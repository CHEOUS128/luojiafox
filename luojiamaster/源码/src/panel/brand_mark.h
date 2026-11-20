#pragma once

#include "luojiamaster_brand.h"
#include "panel/fox_shape.h"
#include <algorithm>
#include <array>

namespace luojiamaster::brand {
constexpr ImU32 kOrange = IM_COL32(244, 121, 32, 255);
constexpr float kAspect = shape::kMarkWidth / shape::kMarkHeight;

template<size_t N>
inline void Stroke(ImDrawList *draw, const ImVec2 (&outline)[N], ImVec2 start, float scale, ImU32 color) {
    std::array<ImVec2, N> points;
    for (size_t i = 0; i < N; ++i) {
        points[i] = {start.x + scale * outline[i].x, start.y + scale * outline[i].y};
    }
    draw->AddConcavePolyFilled(points.data(), static_cast<int>(N), color);
}

inline void DrawMark(ImDrawList *draw, ImVec2 start, float height, ImU32 color) {
    const float scale = height / shape::kMarkHeight;
    Stroke(draw, shape::kStroke0, start, scale, color);
    Stroke(draw, shape::kStroke1, start, scale, color);
    Stroke(draw, shape::kStroke2, start, scale, color);
    Stroke(draw, shape::kStroke3, start, scale, color);
    Stroke(draw, shape::kStroke4, start, scale, color);
}

// Draw directly in unused header space. No ImGui item, focus target or input rectangle.
inline void DrawHudMarkInGap(ImDrawList *draw, float left, float right, float cy, float height) {
    if (height < 13.f || right - left < kAspect * height) return;
    const ImVec2 corner{right - kAspect * height, cy - height * .5f};
    DrawMark(draw, {corner.x + 1.f, corner.y + 1.f}, height, IM_COL32(14, 23, 26, 184));
    DrawMark(draw, corner, height, IM_COL32(249, 249, 246, 246));
}

// The wordmark uses the current ImGui font and the original fox silhouette.
inline void Masthead(bool large) {
    auto *draw = ImGui::GetWindowDrawList();
    const auto start = ImGui::GetCursorScreenPos();
    const float em = ImGui::GetFontSize();
    const float logoHeight = (large ? 3.9f : 1.8f) * em;
    const float titleSize = (large ? 2.55f : 1.35f) * em;
    const float gap = (large ? 1.05f : .65f) * em;
    const float tx = start.x + logoHeight * kAspect + gap;
    DrawMark(draw, start, logoHeight, kOrange);
    draw->AddText(ImGui::GetFont(), titleSize, {tx, start.y}, IM_COL32_WHITE, LUOJIAMASTER_PRODUCT);
    if (large) {
        draw->AddText(ImGui::GetFont(), em, {tx, start.y + titleSize + .12f * em},
                      IM_COL32(210, 221, 220, 255), LUOJIAMASTER_TEAM);
        draw->AddText({tx, start.y + titleSize + 1.32f * em},
                      IM_COL32(170, 187, 185, 255), LUOJIAMASTER_AUTHOR);
    }
    ImGui::Dummy({1.f, large ? titleSize + 2.45f * em : logoHeight});
}

inline void MenuSignature() {
    const float em = ImGui::GetFontSize();
    const auto previousEnd = ImGui::GetItemRectMax();
    const float rowTop = ImGui::GetItemRectMin().y;
    const float edge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const float height = 1.22f * em;
    const float wordWidth = ImGui::CalcTextSize(LUOJIAMASTER_PRODUCT).x;
    const float creditWidth = ImGui::CalcTextSize(LUOJIAMASTER_AUTHOR).x;
    const float width = height * kAspect + wordWidth + creditWidth + 1.4f * em;
    ImVec2 start{edge - width, rowTop};
    if (start.x <= previousEnd.x + em) {
        start = ImGui::GetCursorScreenPos();
        ImGui::Dummy({1.f, height});
    }
    auto *draw = ImGui::GetWindowDrawList();
    DrawMark(draw, start, height, kOrange);
    const float tx = start.x + height * kAspect + .55f * em;
    draw->AddText({tx, start.y}, IM_COL32_WHITE, LUOJIAMASTER_PRODUCT);
    draw->AddText({tx + wordWidth + .85f * em, start.y},
                  IM_COL32(200, 212, 211, 255), LUOJIAMASTER_AUTHOR);
}
} // namespace luojiamaster::brand
