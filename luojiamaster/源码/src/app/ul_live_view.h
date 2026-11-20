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

inline constexpr bool IsCupMapSeat(int id) {
    return id == 1 || id == 3 || id == 4 || id == 101 || id == 103 || id == 104;
}

// Display choices for one signed-in operator. The menu and preferred camera
// here belong to the league HUD; both cup seasons share its map toggle.
struct UlHudView {
    int identity = 0;
    bool menu_open = false;
    bool industrial_main = false;
    bool map_open = false;

    // Consume the closing edge too, so Esc cannot open the menu underneath.
    bool HandleMapKeys(bool m_pressed, bool escape_pressed, bool text_input, bool other_menu_open = false) {
        if (map_open) {
            if (!text_input && (m_pressed || escape_pressed)) map_open = false;
            return true;
        }
        if (identity <= 0 || menu_open || other_menu_open || text_input || escape_pressed || !m_pressed) return false;
        map_open = true;
        return true;
    }

    bool SelectIdentity(int seat) {
        if (identity == seat) return false;
        identity = seat;
        menu_open = false;
        map_open = false;
        industrial_main = seat == 1 || seat == 101;
        return true;
    }
};
} // namespace RoboGo::app
