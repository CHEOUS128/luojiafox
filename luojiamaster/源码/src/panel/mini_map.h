#pragma once

#include "main_window.h"
#include "io/mqtt_client.h"

void ShowMiniMap(const State &state,
                 RoboGo::io::MqttClient &mqtt_client,
                 ImTextureRef mini_map_texture_ref,
                 const ImVec2 &mini_map_texture_size);
