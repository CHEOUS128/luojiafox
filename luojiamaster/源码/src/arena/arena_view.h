#pragma once
#include "imgui.h"
#include "arena/replay_marker.h"
#include <array>
#include <vector>

namespace RoboGo::arena {
enum class Mode { Flat, Light, Detailed, Unavailable };
// Local recording cards use the same screen coordinates as the sample pieces.
struct ReplayPin {
    ImVec2 screen{};
    bool has_position{}, inside{};
    int age_seconds{};
};
struct ReplayLabelBox {
    ImVec2 lo{}, hi{}; // final nameplate after label avoidance
    ImVec2 piece_lo{}, piece_hi{};
};
struct ReplayCanvas {
    ImVec2 lo{}, hi{};
    std::array<ReplayPin,3> pins{}; // seats 1, 3, 4
    std::array<ReplayLabelBox,8> labels{};
    int label_count{};
    bool map_dragging{};
};
Mode Draw(const ImVec2& size, Mode requested, ImTextureID flat, double benchmark_seconds = -1,
          const std::vector<ReplayMarker>& markers = {}, ReplayCanvas* canvas = nullptr,
          int replay_team = 0, bool pointer_on_card = false);
void ResetCamera();
const char* Status();
}
