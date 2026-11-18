#pragma once
#include <chrono>

namespace RoboGo::app {

// Freshness here is local display availability, not the camera capture time.
// A new upload is drawable in this frame, before Present can commit its timestamp.
inline bool CanDrawVideoFrame(bool texture_ready, bool just_uploaded,
                              std::chrono::steady_clock::time_point last_presented,
                              std::chrono::steady_clock::duration timeout,
                              std::chrono::steady_clock::time_point now) {
    return texture_ready &&
           (just_uploaded || (last_presented != std::chrono::steady_clock::time_point::min() &&
                              now - last_presented <= timeout));
}

// Display-only choices for one signed-in RMUL seat. Match actions never
// implicitly acquire the official keyboard/mouse channel.
struct UlHudView {
    int identity = 0;
    bool menu_open = false;
    bool industrial_main = false;

    bool SelectIdentity(int seat) {
        if (identity == seat) return false;
        identity = seat;
        menu_open = false;
        industrial_main = seat == 1 || seat == 101;
        return true;
    }
};
} // namespace RoboGo::app
