#include "panel/replay_card_layout.h"
#include <cmath>
#include <iostream>

using luojiamaster::replay_cards::Layout;
using luojiamaster::replay_cards::Place;
using RoboGo::arena::ReplayCanvas;

bool SeparatedAndInside(const Layout &layout, const ReplayCanvas &canvas) {
    for (size_t i = 0; i < layout.boxes.size(); ++i) {
        const auto &box = layout.boxes[i];
        if (!(box.GetWidth() > 0 && box.GetHeight() > 0 && box.Min.x >= canvas.lo.x &&
              box.Min.y >= canvas.lo.y && box.Max.x <= canvas.hi.x && box.Max.y <= canvas.hi.y)) return false;
        for (size_t j = 0; j < i; ++j)
            if (box.Overlaps(layout.boxes[j])) return false;
    }
    return true;
}

bool RunCases(ImVec2 origin, ImVec2 size, float font) {
    ReplayCanvas canvas;
    canvas.lo = origin;
    canvas.hi = {origin.x + size.x, origin.y + size.y};
    for (const ImVec2 center : {ImVec2{.5f,.53f}, ImVec2{.02f,.02f}, ImVec2{.98f,.97f}}) {
        Layout layout;
        for (int i = 0; i < 3; ++i) {
            auto &pin = canvas.pins[i];
            pin.has_position = pin.inside = true;
            pin.screen = {origin.x + size.x * center.x + i, origin.y + size.y * center.y};
        }
        Place(layout,canvas,-1,-1,font);
        if (!SeparatedAndInside(layout,canvas)) return false;
        if (center.x == .5f) {
            // A separate sample vehicle's number can occupy the preferred spot.
            const auto q = layout.boxes[0].GetCenter();
            canvas.label_count = 1;
            canvas.labels[0] = {{q.x-24,q.y-10},{q.x+24,q.y+10},
                                {q.x-40,q.y-11},{q.x-25,q.y+11}};
            Place(layout,canvas,-1,-1,font);
            for (const auto& box : layout.boxes)
                if (box.Overlaps({canvas.labels[0].lo,canvas.labels[0].hi})) return false;
            canvas.label_count = 0;
        }
        const auto small = layout.boxes[1];
        ImGui::GetIO().MousePos = small.GetCenter();
        canvas.pins[1].screen.x += 43;
        Place(layout,canvas,-1,1,font);
        if (layout.boxes[1].Min.x != small.Min.x || layout.boxes[1].Min.y != small.Min.y) return false;
        Place(layout,canvas,1,1,font);
        if (layout.Hit(ImGui::GetIO().MousePos,1) != 1 ||
            layout.boxes[1].GetWidth() < small.GetWidth()*1.8f ||
            layout.boxes[1].GetWidth() > size.x*.51f || !SeparatedAndInside(layout,canvas)) return false;
        Place(layout,canvas,2,-1,font);
        if (layout.boxes[1].GetWidth() > small.GetWidth()+1.f ||
            layout.Hit(layout.boxes[2].GetCenter(),2) != 2 || !SeparatedAndInside(layout,canvas)) return false;
        for (auto &pin : canvas.pins) pin.has_position = pin.inside = false;
        Place(layout,canvas,-1,-1,font);
        if (!SeparatedAndInside(layout,canvas)) return false;
        for (const auto &box : layout.boxes)
            if (box.GetCenter().y < origin.y + size.y*.75f) return false;
    }
    return true;
}

int main() {
    ImGui::CreateContext();
    const bool ok = RunCases({0,90},{1920,940},20) && RunCases({31,104},{1500,760},22);
    ImGui::DestroyContext();
    std::cout << (ok ? "PASS" : "FAIL") << ": local replay cards, cluster, edges, hover, focus, docking\n";
    return ok ? 0 : 1;
}
