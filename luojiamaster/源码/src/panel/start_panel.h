#pragma once

#include "main_window.h"
#include "io/mqtt_client.h"

void StartPanel(const State &state, RoboGo::io::MqttClient &mqtt_client);
