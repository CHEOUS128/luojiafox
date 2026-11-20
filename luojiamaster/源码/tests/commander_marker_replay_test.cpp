#include "app/commander_marker_replay.h"
#include "arena/replay_projection.h"
#include "arena/replay_piece.h"
#include "arena/replay_role_ink.h"
#include <string_view>
#include <cmath>
#include <iostream>

int main() {
    using Replay = RoboGo::app::CommanderMarkerReplay;
    using namespace std::chrono_literals;
    const Replay::Time start{};
    Replay replay;
    replay.Start(start);
    if (!replay.Visible(start + 249ms).empty()) return 1;
    const auto opening = replay.Visible(start + 250ms);
    if (opening.size() != 8) return 2;
    constexpr std::array numbers{1, 3, 4, 7};
    for (std::size_t slot = 0; slot < opening.size(); ++slot) {
        const auto& m = opening[slot];
        if (m.team != static_cast<int>(slot / 4) || m.number != numbers[slot % 4] ||
            m.age_seconds != 0 || (m.u < .5f) != (m.team == 0)) return 3;
    }
    replay.ToggleBlue3(start + 1750ms);
    const auto paused = replay.Visible(start + 1750ms);
    const auto stale = replay.Visible(start + 4750ms);
    if (stale.size() != 8 || stale[5].age_seconds != 3 ||
        stale[5].u != paused[5].u || stale[5].v != paused[5].v) return 4;
    if (stale[1].u == paused[1].u || stale[1].age_seconds != 0) return 5;
    if (replay.Visible(start + 11749ms).size() != 8) return 6;
    const auto hidden = replay.Visible(start + 11750ms);
    if (hidden.size() != 7) return 7;
    for (const auto& m : hidden) if (m.team == 1 && m.number == 3) return 8;
    replay.ToggleBlue3(start + 12s);
    const auto recovered = replay.Visible(start + 12s);
    if (recovered.size() != 8 || recovered[5].age_seconds != 0) return 9;
    Replay reference;
    reference.Start(start);
    const auto timeline = reference.Visible(start + 12s);
    for (std::size_t i = 0; i < 8; ++i)
        if (timeline[i].u != recovered[i].u || timeline[i].v != recovered[i].v) return 10;
    replay.ToggleBlue3(start + 13s);
    replay.Start(start + 16s);
    if (replay.Blue3Paused() || !replay.Visible(start + 16s).empty()) return 11;
    const auto restarted = replay.Visible(start + 16s + 250ms);
    if (restarted.size() != opening.size()) return 12;
    for (std::size_t i = 0; i < opening.size(); ++i)
        if (restarted[i].u != opening[i].u || restarted[i].v != opening[i].v) return 13;
    // R pad centers read independently from the mesh top view (740,356) and
    // the 903 x 499 rule figure (844,395). This pad was held out of the fit.
    const float pad_u = .5f + (740.f - 400.f) / (27.f * 28.f);
    const float pad_v = .5f + (356.f - 229.f) / (27.f * 15.f);
    const auto pad = RoboGo::arena::RuleFigurePoint(pad_u, pad_v);
    if (!(std::hypot(pad.x - 844.f, pad.y - 395.f) < 2.f)) return 14;
    // Independent reference projection of the same point in pixels at a
    // known yaw and tilt. The top-view scale stays 43.47939 px/m at 1.5x zoom,
    // so this pad can project past the canvas when the arena is rotated.
    const auto camera = RoboGo::arena::ReplayProjection::ForCanvas(903.f, 499.f, .36f, .68f, 1.5f);
    const auto side = camera.SamplePlaneUV(pad_u, pad_v);
    if (!(std::hypot(side.x * 903.f - 891.8755f, side.y * 499.f - 548.3062f) < .1f)) return 15;
    using namespace RoboGo::arena;
    for(const auto& model:pieces::Models()) {
        float max_radius=0;
        for(const auto vertex:model.outline) max_radius=std::max(max_radius,std::hypot(vertex.x,vertex.z));
        if(std::abs(2*max_radius-.60f)>.0001f || model.height<=0 || model.triangles.empty() ||
           model.triangles.size()%3) return 16;
        for(float yaw:{0.f,.6f,1.5f,2.4f}) {
            const auto span=pieces::InkSize(model,std::cos(yaw),std::sin(yaw));
            if(span.x <.30f || span.x>.46f || span.z>=span.x) return 20;
        }
    }
    const auto screen_diameter=[](float zoom) {
        const auto projection=ReplayProjection::ForCanvas(903,499,0,0,zoom);
        return 903*(projection.SamplePlaneUV(.5f+pieces::radius/28.f,.5f).x -
                    projection.SamplePlaneUV(.5f-pieces::radius/28.f,.5f).x);
    };
    if(std::abs(screen_diameter(2)-2*screen_diameter(1))>.001f) return 17;
    if(side.y<=camera.SamplePlaneUV(pad_u,pad_v,.36f).y) return 18;
    if(std::abs(opening.front().floor_y-.097f)>.001f ||
       std::abs(reference.Visible(start+3250ms)[1].floor_y+.001f)>.001f) return 19;
    if(pieces::RoleCharacter(1)!=U'重' || pieces::RoleCharacter(3)!=U'步' ||
       pieces::RoleCharacter(4)!=U'步' || pieces::RoleCharacter(7)!=U'哨') return 21;
    for(auto ink:pieces::role_ink) if(std::string_view(ink).size()!=32*32) return 22;
    std::cout << "PASS: eight local identities, ages, expiry, resume, new session, held-out pad, "
                 "tilted zoom, 60 cm footprints, Chinese role inscriptions and sample surface height\n";
}
