#pragma once
#include "arena/arena_view.h"
#include "imgui_internal.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace luojiamaster::replay_cards {

struct Layout {
    std::array<ImRect, 3> boxes{};
    std::array<int, 3> sides{-1, -1, -1};
    int pressed = -1;
    bool valid = false;

    int Hit(ImVec2 p, int focused) const {
        if (!valid) return -1;
        if (focused >= 0 && boxes[focused].Contains(p)) return focused;
        for (int i = 2; i >= 0; --i)
            if (boxes[i].Contains(p)) return i;
        return -1;
    }
};

inline ImVec2 Closest(const ImRect &rect, ImVec2 p) {
    return {std::clamp(p.x, rect.Min.x, rect.Max.x), std::clamp(p.y, rect.Min.y, rect.Max.y)};
}

inline float Overlap(const ImRect &a, const ImRect &b) {
    return std::max(0.f, std::min(a.Max.x,b.Max.x) - std::max(a.Min.x,b.Min.x))
         * std::max(0.f, std::min(a.Max.y,b.Max.y) - std::max(a.Min.y,b.Min.y));
}

inline ImRect Fit(ImVec2 pos, ImVec2 size, const ImRect &bounds) {
    pos.x = std::clamp(pos.x, bounds.Min.x, std::max(bounds.Min.x, bounds.Max.x - size.x));
    pos.y = std::clamp(pos.y, bounds.Min.y, std::max(bounds.Min.y, bounds.Max.y - size.y));
    return {pos, {pos.x + size.x, pos.y + size.y}};
}

// Reserve the hovered/focused card first, then choose the nearest free side for
// the others. Score visibility before leader length, and favor the previous
// side so two nearby seats do not keep trading places.
inline void Place(Layout &layout, const RoboGo::arena::ReplayCanvas &canvas, int focused,
                  int hovered, float font) {
    ImRect bounds(canvas.lo, canvas.hi);
    const float margin = std::max(0.f, std::min({font * .4f, bounds.GetWidth() * .03f, bounds.GetHeight() * .03f}));
    bounds.Expand(-margin);
    const float w = std::max(1.f, bounds.GetWidth()), h = std::max(1.f, bounds.GetHeight());
    const float width = std::min({font * 9.5f, w * .23f, h * .38f});
    const float height = std::min(h * .24f, width * 9.f / 16.f + font * 1.5f);
    const float gap = font * 1.65f;
    std::array<int,3> order{0,1,2};
    const auto priority = [&](int i) { return i == hovered ? 0 : i == focused ? 1 : i + 2; };
    std::sort(order.begin(),order.end(),[&](int a,int b){return priority(a) < priority(b);});
    for (int n = 0; n < 3; ++n) {
        const int slot = order[n];
        const auto &pin = canvas.pins[slot];
        const bool on_map = pin.inside && pin.has_position;
        const float factor = slot == focused ? std::min({2.5f, w * .46f / width, h * .51f / height}) : 1.f;
        const ImVec2 size{width * factor, height * factor};
        const ImRect prev = layout.boxes[slot];
        if (layout.valid && slot == hovered && on_map) {
            // Retain the exact old corners when the box size has not changed.
            // Subtracting its rounded screen coordinates can differ by a few
            // ULPs from the original computed width, even at a fixed DPI.
            if (std::abs(prev.GetWidth()-size.x) < .01f &&
                std::abs(prev.GetHeight()-size.y) < .01f && bounds.Contains(prev)) continue;
            // Resize about the pointer when a held card is enlarged/shrunk.
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const float fx = std::clamp((mouse.x - prev.Min.x)/std::max(1.f,prev.GetWidth()),0.f,1.f);
            const float fy = std::clamp((mouse.y - prev.Min.y)/std::max(1.f,prev.GetHeight()),0.f,1.f);
            layout.boxes[slot] = Fit({prev.Min.x - fx*(size.x-prev.GetWidth()),
                                      prev.Min.y - fy*(size.y-prev.GetHeight())},size,bounds);
            continue;
        }
        // Without a visible position, dock the recording at the lower edge.
        // A disconnected picture with a known position stays with its own piece.
        const ImVec2 p = on_map ? pin.screen :
            ImVec2(bounds.Min.x + (slot+.5f)*w/3.f,bounds.Max.y + gap);
        const std::array<ImVec2,8> candidates{{
            {p.x+gap,p.y-size.y-gap}, {p.x-size.x-gap,p.y-size.y-gap},
            {p.x+gap,p.y+gap}, {p.x-size.x-gap,p.y+gap},
            {p.x-size.x*.5f,p.y-size.y-gap}, {p.x-size.x*.5f,p.y+gap},
            {p.x+gap,p.y-size.y*.5f}, {p.x-size.x-gap,p.y-size.y*.5f}
        }};
        float best = std::numeric_limits<float>::max();
        bool found_clear = false;
        int best_side = -1;
        ImRect chosen;
        const auto consider = [&](ImVec2 pos,int side) {
            const ImRect candidate = Fit(pos,size,bounds);
            const ImVec2 attach = Closest(candidate,p);
            float score = std::hypot(attach.x-p.x,attach.y-p.y);
            if (side != layout.sides[slot]) score += font * 1.4f;
            if (!on_map) score += std::abs(candidate.GetCenter().x - p.x);
            float covered = 0.f;
            for (int i = 0; i < canvas.label_count; ++i) {
                const auto &label = canvas.labels[i];
                ImRect text(label.lo,label.hi), piece(label.piece_lo,label.piece_hi);
                text.Expand(font * .14f);
                piece.Expand(font * .14f);
                covered += Overlap(candidate,text) + Overlap(candidate,piece);
            }
            for (const auto &other : canvas.pins) {
                if (!other.inside || !other.has_position) continue;
                const auto pt = other.screen;
                score += Overlap(candidate, {{pt.x-gap,pt.y-gap},{pt.x+gap,pt.y+gap}}) * 4.f;
            }
            for (int j = 0; j < n; ++j) {
                ImRect occupied = layout.boxes[order[j]];
                occupied.Expand(font*.3f);
                covered += Overlap(candidate,occupied) * 2.f;
            }
            const bool clear = covered < .01f;
            if (found_clear && !clear) return;
            if (!found_clear && clear) { found_clear = true; best = std::numeric_limits<float>::max(); }
            score += covered * 12.f;
            if (score < best) { best = score; chosen = candidate; best_side = side; }
        };
        for (int i = 0; i < static_cast<int>(candidates.size()); ++i) consider(candidates[i],i);
        for (int j = 0; j < n; ++j) {
            const ImRect b = layout.boxes[order[j]];
            const float space = font * .4f;
            consider({b.Min.x-size.x-space,b.Min.y},8+order[j]*4);
            consider({b.Max.x+space,b.Min.y},9+order[j]*4);
            consider({b.Min.x,b.Min.y-size.y-space},10+order[j]*4);
            consider({b.Min.x,b.Max.y+space},11+order[j]*4);
        }
        // Sliding a card along the nearest edge is often enough to reveal a
        // nameplate. Only search the remaining canvas if all the obvious sides
        // are blocked. Evaluate the whole grid to choose the shortest leader.
        if (!found_clear) {
            const float step = std::max(font, std::max(w,h) / 42.f);
            const int nx = 1 + static_cast<int>(std::ceil((w-size.x)/step));
            const int ny = 1 + static_cast<int>(std::ceil((h-size.y)/step));
            for (int row = 0; row < ny; ++row)
                for (int col = 0; col < nx; ++col)
                    consider({bounds.Min.x+col*step,bounds.Min.y+row*step},32+row*64+col);
        }
        layout.boxes[slot] = chosen;
        layout.sides[slot] = best_side;
    }
    layout.valid = true;
}

} // namespace luojiamaster::replay_cards
