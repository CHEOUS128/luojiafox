#pragma once

#include "arena/arena_view.h"
#include "app/commander_marker_replay.h"
#include "panel/teammate_panel.h"
#include "panel/replay_card_layout.h"

struct CommanderDeskState {
    int team = 0;
    int focused = -1;
    bool floating_cards = true;
    luojiamaster::replay_cards::Layout cards;
    bool show_markers = true;
    RoboGo::app::CommanderMarkerReplay replay;
    std::array<float, 3> zoom = {1.f, 1.f, 1.f};
    // Size of an unselected picture in actual pixels, excluding its title and padding.
    ImVec2 thumbnail_pixels{};

    int PreviewWidth(size_t slot, int source_width, int source_height) const {
        if (source_width <= 0 || source_height <= 0 || thumbnail_pixels.x < 1.f || thumbnail_pixels.y < 1.f ||
            focused == static_cast<int>(slot) || ((floating_cards || focused < 0) && zoom[slot] > 1.f)) return 0;
        const float fit = std::min(thumbnail_pixels.x, thumbnail_pixels.y * source_width / source_height);
        // A small bucket avoids texture/scale-context churn as the operator resizes.
        const int width = (static_cast<int>(std::ceil(fit)) + 63) / 64 * 64;
        return width < source_width ? width : 0;
    }

    RoboGo::arena::Mode requested_mode = RoboGo::arena::Mode::Light;
};

struct CommanderDeskActions {
    bool return_to_events = false;
    int toggle_feed = -1;
    unsigned drawn_mask = 0;
    std::size_t map_marker_count = 0;
    RoboGo::arena::Mode actual_mode = RoboGo::arena::Mode::Unavailable;
};

CommanderDeskActions ShowCommanderDesk(CommanderDeskState &desk, const TeammateViewState &feeds,
                                       ImTextureID field_plan, RoboGo::app::CommanderMarkerReplay::Time frame_time,
                                       double benchmark_seconds = -1);
