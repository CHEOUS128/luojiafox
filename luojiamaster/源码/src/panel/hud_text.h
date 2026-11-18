#pragma once

#include <pch.h>
#include <cstdarg>

namespace hud {

constexpr ImU32 white = IM_COL32(247, 248, 250, 255);
constexpr ImU32 muted = IM_COL32(217, 224, 228, 255);

// A one-pixel edge follows the glyphs. Everything between labels stays transparent.
inline void Outline(ImDrawList *list, ImVec2 pos, ImU32 color, const char *text,
                    float font_size = 0.f, float wrap_width = 0.f) {
    auto *font = ImGui::GetFont();
    if (font_size <= 0.f) font_size = ImGui::GetFontSize();
    constexpr ImU32 edge = IM_COL32(10, 14, 18, 230);
    for (const auto delta : {ImVec2{-1, 0}, ImVec2{1, 0}, ImVec2{0, -1}, ImVec2{0, 1}})
        list->AddText(font, font_size, {pos.x + delta.x, pos.y + delta.y}, edge, text, nullptr, wrap_width);
    list->AddText(font, font_size, pos, color, text, nullptr, wrap_width);
}

inline void Label(const char *text, ImU32 color = white, bool wrap = false) {
    const float width = wrap ? std::max(1.f, ImGui::GetContentRegionAvail().x) : 0.f;
    const auto size = ImGui::CalcTextSize(text, nullptr, false, wrap ? width : -1.f);
    const auto at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    if (ImGui::IsItemVisible())
        Outline(ImGui::GetWindowDrawList(), at, color, text, ImGui::GetFontSize(), width);
}

inline void Text(ImU32 color, const char *fmt, ...) IM_FMTARGS(2);
inline void Text(ImU32 color, const char *fmt, ...) {
    const char *text;
    va_list args;
    va_start(args, fmt);
    ImFormatStringToTempBufferV(&text, nullptr, fmt, args);
    va_end(args);
    Label(text, color);
}
} // namespace hud
