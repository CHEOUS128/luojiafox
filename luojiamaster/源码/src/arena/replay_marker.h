#pragma once

namespace RoboGo::arena {

// Positions in the local spectator demo, normalized left-to-right and top-to-bottom.
struct ReplayMarker {
    int team;
    int number;
    float u, v;
    int age_seconds;
    float floor_y{}; // local model surface under this fictional sample
};

} // namespace RoboGo::arena
