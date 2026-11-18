#pragma once

#include "main_window.h"
#include "io/mqtt_client.h"

enum class MenuAction { None, Resume, SwitchLayout, ReturnToEvents };
void ResetStartPanelSession(RoboGo::app::Competition competition);
void PositionMatchMenu();
ImRect MatchMenuBounds();
MenuAction StartPanel(const State &state, RoboGo::io::MqttClient &client, bool local_trial,
                      bool over_ul_desk = false, bool waiting_for_video = false);
