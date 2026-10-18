#pragma once

#include <pch.h>

#include "io/mqtt_client.h"

bool is_in_game_mode();

void enter_game_mode();

void set_game_input_capture_handler(std::function<void(bool enabled)> handler);

void update_keyboard_mouse_state(RoboGo::io::MqttClient &mqtt_client);

// 平台层（Windows）每帧在“把光标拉回窗口中心”之前调用，累积真实的鼠标位移增量。
// 这样光标被重置也不会丢失移动量——这是官方客户端那种“无限旋转”手感的关键。
void add_raw_mouse_delta(float dx, float dy);

// 平台层在锁定光标后调用，告知使用原始位移（true）还是 ImGui 增量（false）
void set_use_raw_mouse(bool enabled);

float get_mouse_sensitivity();

void set_mouse_sensitivity(float sensitivity);

bool is_marker_mode();

bool is_marker_mode_include_sentry();

void set_marker_mode_include_sentry(bool include);

void enter_marker_mode();

void exit_marker_mode();

void send_map_marker(RoboGo::io::MqttClient &mqtt_client, float map_x, float map_y);
