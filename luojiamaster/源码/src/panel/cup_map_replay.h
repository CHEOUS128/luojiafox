#pragma once
#include "app/commander_marker_replay.h"
#include "arena/replay_projection.h"
#include "panel/replay_card_layout.h"

// This view owns only the fictional local replay. No network input is accepted.
struct CupMapReplay {
    using Samples = RoboGo::app::CommanderMarkerReplay;
    using Time = Samples::Time;
    int seat = 0;
    int focused = -1;
    luojiamaster::replay_cards::Layout cards;
    std::array<float,3> zoom{1.f,1.f,1.f};
    Samples samples;

    static std::array<int,2> PeerSeats(int seat) {
        switch (seat) {
        case 1: return {3,4};     case 101: return {103,104};
        case 3: return {1,4};     case 103: return {101,104};
        case 4: return {1,3};     case 104: return {101,103};
        default: return {};
        }
    }
    void Reset(int new_seat, Time now) {
        seat = new_seat;
        focused = -1;
        cards = {};
        zoom.fill(1.f);
        samples.Start(now);
    }
    void TogglePosition(int id, Time now) { samples.TogglePosition(id,now); }
    bool PositionPaused(int id) const { return samples.PositionPaused(id); }
    const auto& Visible(Time now) { return samples.Visible(now); }

    // Same landmarks as the commander's flat plan. The legacy small map
    // uses the field crop and a fixed orientation for both teams.
    static ImVec2 FigureUV(float u, float v, bool cup_2027, bool blue) {
        if (!cup_2027) return {u,v};
        const auto p = RoboGo::arena::RuleFigurePoint(u,v);
        const ImVec2 uv{p.x/903.f,p.y/499.f};
        return blue ? ImVec2{1.f-uv.x,1.f-uv.y} : uv;
    }
};

template<class Recordings>
int ReplaySlotFor(const Recordings& recordings, int robot_id) {
    if (robot_id <= 0) return -1;
    for (int i=0; i<recordings.count && i<static_cast<int>(recordings.peers.size()); ++i)
        if (recordings.peers[i].robot_id == robot_id) return i;
    return -1;
}
