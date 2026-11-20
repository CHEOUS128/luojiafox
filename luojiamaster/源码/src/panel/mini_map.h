#pragma once

#include "main_window.h"
#include "io/mqtt_client.h"

void ShowMiniMap(const State &state,
                 RoboGo::io::MqttClient &mqtt_client,
                 ImTextureRef mini_map_texture_ref,
                 const ImVec2 &mini_map_texture_size, CupMapReplay* local_replay = nullptr);

void ShowAllianceVenueMap(ImTextureRef texture, const ImVec2 &size, bool blue);
void ShowAllianceExpandedMap(ImTextureRef texture, const ImVec2 &size, bool blue, bool local_trial);
void ShowCupVenueMap(ImTextureRef texture, const ImVec2 &size, bool blue, bool team_known, CupMapReplay* local_replay = nullptr);
