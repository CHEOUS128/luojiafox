#pragma once
#include "imgui.h"
#include "arena/replay_marker.h"
#include <vector>

namespace RoboGo::arena {
enum class Mode { Flat, Light, Detailed, Unavailable };
Mode Draw(const ImVec2& size, Mode requested, ImTextureID flat, double benchmark_seconds = -1,
          const std::vector<ReplayMarker>& markers = {});
void ResetCamera();
const char* Status();
}
