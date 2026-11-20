#include "panel/replay_card_layout.h"
#include "panel/cup_map_replay.h"
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

bool RunCupReplayCases() {
    using namespace std::chrono_literals;
    CupMapReplay replay;
    struct Recording { int robot_id; };
    struct Recordings {
        std::array<Recording, 3> peers{};
        int count = 0;
    };
    const std::chrono::steady_clock::time_point start{20s};
    for (const int color : {0, 100}) {
        for (const int role : {1, 3, 4}) {
            const auto ids = CupMapReplay::PeerSeats(color + role);
            std::array<int, 2> expected{};
            int index = 0;
            for (const int candidate : {1,3,4})
                if (candidate != role) expected[index++] = color + candidate;
            if (ids != expected) return false;
            Recordings recordings;
            recordings.peers[0].robot_id = ids[1];
            recordings.count = 1;
            if (ReplaySlotFor(recordings, ids[0]) != -1 || ReplaySlotFor(recordings, ids[1]) != 0)
                return false;
            recordings.peers[1].robot_id = ids[0];
            recordings.count = 2;
            if (ReplaySlotFor(recordings, ids[0]) != 1 || ReplaySlotFor(recordings, color + role) != -1)
                return false;
            replay.Reset(color + role, start);
            if (!replay.Visible(start).empty()) return false;
            const auto now = start + 1s;
            if (replay.Visible(now).size() != 8) return false;
            const auto paused = ids[0];
            replay.TogglePosition(paused, now);
            const auto lookup = [&](const auto &rows, int id) {
                return std::find_if(rows.begin(), rows.end(), [id](const auto &row) {
                    return row.number + 100 * row.team == id;
                });
            };
            const auto& stale = replay.Visible(now + 3s);
            const auto held = lookup(stale, paused);
            if (held == stale.end() || held->age_seconds != 3) return false;
            const auto& expired = replay.Visible(now + 10s);
            if (expired.size() != 7 || lookup(expired, paused) != expired.end()
                || lookup(expired,ids[1]) == expired.end()) return false;
            replay.TogglePosition(paused, now + 11s);
            const auto& resumed = replay.Visible(now + 12s);
            if (resumed.size() != 8 || lookup(resumed,paused) == resumed.end()) return false;
        }
    }
    for (const auto point : {ImVec2{.1f,.7f}, ImVec2{.71f,.23f}}) {
        const auto red = CupMapReplay::FigureUV(point.x, point.y, true, false);
        const auto blue = CupMapReplay::FigureUV(point.x, point.y, true, true);
        const auto old_red = CupMapReplay::FigureUV(point.x, point.y, false, false);
        const auto old_blue = CupMapReplay::FigureUV(point.x, point.y, false, true);
        if (std::abs(red.x + blue.x - 1.f) > 1e-5f || std::abs(red.y + blue.y - 1.f) > 1e-5f ||
            old_red.x != point.x || old_red.y != point.y || old_blue.x != point.x || old_blue.y != point.y)
            return false;
    }
    Layout layout;
    ReplayCanvas canvas;
    canvas.lo = {100,50};
    canvas.hi = {1670,930};
    for (auto &pin : canvas.pins) {
        pin.has_position = pin.inside = true;
        pin.screen = {450,580};
    }
    Place(layout,canvas,-1,-1,20,2);
    if (layout.boxes[0].Overlaps(layout.boxes[1])) return false;
    const auto first = layout.boxes[0];
    ImGui::GetIO().MousePos = first.GetCenter();
    canvas.pins[0].screen.x += 250.f;
    Place(layout,canvas,-1,0,20,2);
    if (layout.boxes[0].Min.x != first.Min.x || layout.boxes[0].Min.y != first.Min.y) return false;
    Place(layout,canvas,0,0,20,2);
    if (layout.boxes[0].GetWidth() <= first.GetWidth() * 1.8f || layout.boxes[0].Overlaps(layout.boxes[1]))
        return false;
    for (auto &pin : canvas.pins) pin.has_position = pin.inside = false;
    Place(layout,canvas,-1,-1,20,2);
    for (int i = 0; i < 2; ++i)
        if (layout.boxes[i].GetCenter().y < canvas.lo.y + (canvas.hi.y - canvas.lo.y)*.75f) return false;
    return true;
}

int main() {
    ImGui::CreateContext();
    const bool ok = RunCases({0,90},{1920,940},20) && RunCases({31,104},{1500,760},22)
                    && RunCupReplayCases();
    ImGui::DestroyContext();
    std::cout << (ok ? "PASS" : "FAIL") << ": local replay cards, cluster, edges, hover, focus, docking\n";
    return ok ? 0 : 1;
}
