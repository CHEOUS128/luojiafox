#pragma once
#include "main_window.h"
#include "panel/cup_map_replay.h"

std::pair<ImVec2,ImVec2> LegacyMapFieldUV(const ImVec2& size);
void DrawCupReplayMini(CupMapReplay& local_replay, const ImRect& field, bool cup_2027);

MainWindowActions ShowCupExpandedMap(ImTextureRef texture, ImVec2 texture_size, bool cup_2027, bool blue,
                                     CupMapReplay* local_replay, const TeammateViewState* recordings);
