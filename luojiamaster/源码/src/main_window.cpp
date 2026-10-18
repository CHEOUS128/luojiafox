#include "main_window.h"
#include "keyboard_mouse_state_manager.h"
#include "logging/logging.h"
#include "panel/bottom_panel.h"
#include "panel/buff_panel.h"
#include "panel/crosshair.h"
#include "panel/custom_video_panel.h"
#include "panel/header_panel.h"
#include "panel/log_panel.h"
#include "panel/mini_map.h"
#include "panel/robot_status_panel.h"
#include "panel/start_panel.h"

namespace {

    std::optional<std::pair<std::string, std::string>> SplitByComma(const std::string &param) {
        size_t comma_pos = param.find(',');
        if (comma_pos != std::string::npos) {
            return std::make_pair(param.substr(0, comma_pos), param.substr(comma_pos + 1));
        }
        return std::nullopt;
    }

    std::optional<uint8_t> ParseByteParam(const std::string &param) {
        try {
            size_t parsed_size = 0;
            const unsigned long value = std::stoul(param, &parsed_size, 10);
            if (parsed_size == param.size() && value <= std::numeric_limits<uint8_t>::max()) {
                return static_cast<uint8_t>(value);
            }
        } catch (const std::exception &) {
        }
        return std::nullopt;
    }

    void AppendLogBuffer(State &state, std::vector<LogEntry> &buffer, int type, const std::string &text) {
        LogEntry log_entry;
        time_t now = time(nullptr);
        tm *timeinfo = localtime(&now);
        strftime(log_entry.time, sizeof(log_entry.time), "%H:%M:%S", timeinfo);
        log_entry.type = type;
        log_entry.text = text;
        log_entry.sequence = state.next_log_sequence++;
        buffer.push_back(log_entry);
        if (buffer.size() > 50) {
            buffer.erase(buffer.begin());
        }
    }

    void EmitUiLog(int type, const char *scope, const std::string &text) {
        switch (type) {
            case 0:
                SPDLOG_INFO("{}{}", scope, text);
                break;
            case 1:
                SPDLOG_WARN("{}{}", scope, text);
                break;
            case 2:
                SPDLOG_ERROR("{}{}", scope, text);
                break;
            default:
                SPDLOG_INFO("{}{}", scope, text);
                break;
        }
    }

    ImVec2 FitImageSize(const ImVec2 &available_size, const ImVec2 &texture_size) {
        if (available_size.x <= 0.0f || available_size.y <= 0.0f || texture_size.x <= 0.0f || texture_size.y <= 0.0f) {
            return ImVec2(0.0f, 0.0f);
        }

        const float image_aspect = texture_size.x / texture_size.y;
        const float available_aspect = available_size.x / available_size.y;
        if (image_aspect > available_aspect) {
            return ImVec2(available_size.x, available_size.x / image_aspect);
        }
        return ImVec2(available_size.y * image_aspect, available_size.y);
    }

    constexpr float kHeroRulerSpacingPx = 200.0f;
    constexpr ImU32 kRulerLineColor = IM_COL32(180, 80, 255, 120);
    constexpr ImU32 kRulerLabelColor = IM_COL32(255, 255, 255, 180);

    void DrawHeroRulerLines(ImDrawList *draw_list,
                            const ImVec2 &image_min,
                            const ImVec2 &image_max,
                            const ImVec2 &video_size,
                            float spacing_px) {
        if (video_size.y <= 0.0f) return;
        const float scale_y = (image_max.y - image_min.y) / video_size.y;
        for (int i = 1;; ++i) {
            const float screen_y = image_min.y + i * spacing_px * scale_y;
            if (screen_y >= image_max.y) break;
            draw_list->AddLine(ImVec2(image_min.x, screen_y), ImVec2(image_max.x, screen_y),
                               kRulerLineColor, 1.0f);
            const std::string label = std::to_string(static_cast<int>(i * spacing_px));
            draw_list->AddText(ImVec2(image_min.x + 4.0f, screen_y + 2.0f), kRulerLabelColor, label.c_str());
        }
    }

}// namespace

void AppendVehicleLog(State &state, int type, const std::string &text) {
    AppendLogBuffer(state, state.vehicle_logs, type, text);
    EmitUiLog(type, "本车日志: ", text);
}

void AppendSystemLog(State &state, int type, const std::string &text) {
    AppendLogBuffer(state, state.system_logs, type, text);
    EmitUiLog(type, "系统日志: ", text);
}

State CreateInitialState() {
    State state;
    for (size_t i = 0; i < std::size(state.red_robots); i++) {
        RobotStatus robot;
        robot.health = 0;
        robot.bullets = 0;
        state.red_robots[i] = robot;
        state.blue_robots[i] = robot;
    }
    return state;
}

bool ShowMainWindow(const State &state,
                    RoboGo::io::MqttClient &mqtt_client,
                    ImTextureRef video_texture_ref,
                    const ImVec2 &video_size,
                    ImTextureRef custom_video_texture_ref,
                    const ImVec2 &custom_video_size,
                    const bool using_custom_video_as_main,
                    const bool prefer_custom_video_as_main,
                    const bool udp_video_display_ready,
                    const bool custom_video_display_ready,
                    const bool udp_video_inverted,
                    const ImTextureRef mini_map_texture_ref,
                    const ImVec2 &mini_map_texture_size) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGuiStyle &style = ImGui::GetStyle();
    float font_size = ImGui::GetFontSize();

    const ImTextureRef background_video_texture_ref = using_custom_video_as_main
                                                              ? custom_video_texture_ref
                                                              : video_texture_ref;
    const ImVec2 background_video_size = using_custom_video_as_main
                                                 ? custom_video_size
                                                 : video_size;
    const bool background_video_display_ready = using_custom_video_as_main
                                                        ? custom_video_display_ready
                                                        : udp_video_display_ready;
    ImVec2 main_image_min(0.0f, 0.0f);
    ImVec2 main_image_max = viewport->Size;
    if (background_video_display_ready && background_video_texture_ref.GetTexID() != ImTextureID_Invalid) {
        ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
        // 180° 旋转放在 GPU 做（UV 翻转），避免 CPU 逐像素交换整帧 1080p 数据。
        // 这一步原来在解码管线里用 CPU 做，是画面卡顿/操作延迟的主要来源之一。
        const bool main_video_inverted = using_custom_video_as_main ? false : udp_video_inverted;
        const ImVec2 uv0 = main_video_inverted ? ImVec2(1.0f, 1.0f) : ImVec2(0.0f, 0.0f);
        const ImVec2 uv1 = main_video_inverted ? ImVec2(0.0f, 0.0f) : ImVec2(1.0f, 1.0f);
        if (background_video_size.x > 0.0f && background_video_size.y > 0.0f && using_custom_video_as_main) {
            const ImVec2 fitted_size = FitImageSize(viewport->Size, background_video_size);
            main_image_min = ImVec2((viewport->Size.x - fitted_size.x) * 0.5f,
                                    (viewport->Size.y - fitted_size.y) * 0.5f);
            main_image_max = ImVec2(main_image_min.x + fitted_size.x, main_image_min.y + fitted_size.y);
            draw_list->AddImage(background_video_texture_ref, main_image_min, main_image_max, uv0, uv1);
        } else {
            draw_list->AddImage(background_video_texture_ref, ImVec2(0, 0), viewport->Size, uv0, uv1);
        }
    }
    {
        const uint32_t robot_id = state.robot_static_status.has_value()
                                          ? state.robot_static_status->robot_id() : 0;
        const bool is_hero = (robot_id == 1 || robot_id == 101);
        const bool is_deploy_mode = state.deploy_mode_status_sync.has_value()
                                    && state.deploy_mode_status_sync->status() == 1;
        if (is_hero && is_deploy_mode && background_video_display_ready) {
            DrawHeroRulerLines(ImGui::GetBackgroundDrawList(),
                               main_image_min, main_image_max,
                               background_video_size, kHeroRulerSpacingPx);
        }
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, viewport->Size.y * 0.15f));
    ShowHeaderPanel(state);

    float robot_status_panel_height = font_size * 10 + style.SeparatorSize + 4.0f * 4 + style.ItemSpacing.y * 10 + style.WindowPadding.y * 2;
    ImGui::SetNextWindowPos(ImVec2(0, viewport->Size.y - robot_status_panel_height), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x * 0.2f, robot_status_panel_height));
    ShowRobotStatusPanel(state);

    const float left_panel_width = viewport->Size.x * 0.2f;
    const float left_panel_top = viewport->Size.y * 0.15f;
    const float left_panel_total_height = viewport->Size.y - viewport->Size.y * 0.2f - robot_status_panel_height;
    const float left_panel_spacing = style.ItemSpacing.y;
    const float custom_video_panel_height = (left_panel_total_height - left_panel_spacing) * 0.5f;
    const float log_panel_height = left_panel_total_height - left_panel_spacing - custom_video_panel_height;
    const ImTextureRef secondary_video_texture_ref = using_custom_video_as_main
                                                             ? video_texture_ref
                                                             : custom_video_texture_ref;
    const ImVec2 secondary_video_size = using_custom_video_as_main
                                                ? video_size
                                                : custom_video_size;
    const bool secondary_video_display_ready = using_custom_video_as_main
                                                       ? udp_video_display_ready
                                                       : custom_video_display_ready;
    const char *secondary_video_title = using_custom_video_as_main
                                                ? "UDP 视频流"
                                                : "本车视频";
    std::string secondary_video_unavailable_message;
    if (!secondary_video_display_ready) {
        if (using_custom_video_as_main) {
            secondary_video_unavailable_message = prefer_custom_video_as_main
                                                          ? "UDP 视频流当前不可用。"
                                                          : "UDP 视频流当前不可用，主画面正在临时使用 CustomByteBlock 视频流；UDP 恢复后会自动切回。";
        } else if (udp_video_display_ready) {
            secondary_video_unavailable_message = prefer_custom_video_as_main
                                                          ? "CustomByteBlock 视频流当前不可用，恢复后会自动切换到主画面。"
                                                          : "等待 CustomByteBlock 视频流...";
        } else {
            secondary_video_unavailable_message = prefer_custom_video_as_main
                                                          ? "当前两路视频都不可用，主画面偏好为 CustomByteBlock 视频流。"
                                                          : "当前两路视频都不可用，主画面偏好为 UDP 视频流。";
        }
    }

    ImGui::SetNextWindowPos(ImVec2(0, left_panel_top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(left_panel_width, custom_video_panel_height));
    const bool toggle_udp_video_inversion_requested = ShowCustomVideoPanel(
            secondary_video_texture_ref,
            secondary_video_size,
            secondary_video_title,
            secondary_video_display_ready,
            secondary_video_unavailable_message,
            udp_video_inverted);

    ImGui::SetNextWindowPos(ImVec2(0, left_panel_top + custom_video_panel_height + left_panel_spacing), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(left_panel_width, log_panel_height));
    ShowLogPanel(state);

    if (is_in_game_mode()) {
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, viewport->Size.y));
        ShowCrosshair(state);
    } else {
        float width = 750 * ImGui::GetScale();
        float height = 500 * ImGui::GetScale();
        ImGui::SetNextWindowPos(ImVec2((viewport->Size.x - width) / 2, (viewport->Size.y - height) / 2), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(width, height));
        StartPanel(state, mqtt_client);
    }

    float mini_map_width = viewport->Size.x * 0.2f;
    float mini_map_height = mini_map_width / 28 * 15;
    ImGui::SetNextWindowPos(ImVec2(viewport->Size.x - mini_map_width, viewport->Size.y - mini_map_height), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(mini_map_width, mini_map_height));
    ShowMiniMap(state, mqtt_client, mini_map_texture_ref, mini_map_texture_size);

    float buff_panel_width = viewport->Size.x * 0.2f;
    float buff_panel_height = viewport->Size.y - viewport->Size.y * 0.15f - mini_map_height;
    ImGui::SetNextWindowPos(ImVec2(viewport->Size.x - buff_panel_width, viewport->Size.y * 0.15f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(buff_panel_width, buff_panel_height));
    ShowBuffPanel(state);

    ShowBottomPanel(state);

    return toggle_udp_video_inversion_requested;
}

void UpdateState(State &state) {
    auto now = std::chrono::steady_clock::now();
    const std::size_t previous_size = state.active_buffs.size();
    std::erase_if(state.active_buffs,
                  [now](const BuffWithTimestamp &buff_entry) {
                      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                             now - buff_entry.update_time)
                                             .count();
                      return elapsed >= static_cast<int>(buff_entry.buff.buff_left_time());
                  });
    const std::size_t removed = previous_size - state.active_buffs.size();
    if (removed > 0) {
        SPDLOG_DEBUG("清理过期Buff removed={} remaining={}", removed, state.active_buffs.size());
    }
}

void PublishCustomControlIfDue(State &state, RoboGo::io::MqttClient &mqtt_client) {
    const auto now = std::chrono::steady_clock::now();
    if (!IsCustomControlPublishDue(state.last_custom_control_publish_time, now)) {
        return;
    }
    if (!state.robot_static_status.has_value() || !state.global_unit_status.has_value()) {
        return;
    }

    const auto &robot_health = state.global_unit_status->robot_health();
    const auto robot_health_size = robot_health.size();
    const auto half_robot_health_size = robot_health_size / 2;
    if (half_robot_health_size < static_cast<int>(kCustomControlRobotCount)
        || robot_health_size < half_robot_health_size + static_cast<int>(kCustomControlRobotCount)) {
        return;
    }

    std::array<uint16_t, kCustomControlRobotCount> enemy_health{};
    for (size_t k = 0; k < kCustomControlRobotCount; k++) {
        const int enemy_index = half_robot_health_size + static_cast<int>(k);
        const uint16_t hp = static_cast<uint16_t>(std::min(robot_health[enemy_index], uint32_t{65535}));
        enemy_health[k] = hp;
    }

    static uint8_t debug_increment_counter = 0;
    const uint8_t current_debug_counter = debug_increment_counter;
    debug_increment_counter = static_cast<uint8_t>(debug_increment_counter + 1);
    auto data = BuildCustomControlPayload(enemy_health,
                                          state.air_support_custom_control,
                                          current_debug_counter,
                                          now);

    CustomControl custom_control;
    custom_control.set_data(std::string(data.begin(), data.end()));
    std::string serialized;
    if (custom_control.SerializeToString(&serialized)) {
        mqtt_client.publish("CustomControl", serialized);
        state.last_custom_control_publish_time = now;
    }
    SPDLOG_INFO("空中支援呼叫数据: 是否呼叫={}", data[kCustomControlAirSupportCallOffset]);
    if (state.air_support_custom_control.previous_counter_remaining.has_value()) {
        SPDLOG_INFO("空中支援反制次数数据: 剩余反制次数={}",
                    *state.air_support_custom_control.previous_counter_remaining);
    } else {
        SPDLOG_INFO("空中支援反制次数数据: 剩余反制次数=unknown");
    }
    if (data[kCustomControlCounterCooldownOffset] != kInactiveAirSupportCounterCooldown) {
        SPDLOG_INFO("空中支援反制45s倒计时: 剩余={}s", data[kCustomControlCounterCooldownOffset]);
    }
    SPDLOG_INFO("CustomControl 已发出: 英雄={} 工程={} 步兵3={} 步兵4={} 哨兵={} 空中支援={} 反制倒计时={} 计数器={}",
                enemy_health[0],
                enemy_health[1],
                enemy_health[2],
                enemy_health[3],
                enemy_health[4],
                data[kCustomControlAirSupportCallOffset],
                data[kCustomControlCounterCooldownOffset],
                data[kCustomControlDebugCounterOffset]);
}

void HandleMqttMessage(State &state,
                       RoboGo::io::MqttClient &mqtt_client,
                       const std::string &topic,
                       const std::string &payload,
                       const CustomByteBlockPacketHandler &custom_byte_block_packet_handler) {
    (void) mqtt_client;
    // MQTT 入站消息的统一分发入口：
    // 1. 根据 topic 选择对应的 protobuf 类型并反序列化 payload。
    // 2. 把最新数据写入 State，供各个 ImGui 面板读取显示。
    const auto log_parse_failure = [&](const std::string &current_topic) {
        SPDLOG_ERROR("MQTT消息解析失败 topic={} bytes={}", current_topic, payload.size());
    };
    const auto log_parse_success = [&](const std::string &current_topic, const auto &) {
        SPDLOG_DEBUG("MQTT消息解析成功 topic={}", current_topic);
    };

    // 普通同步类消息：解析成功后直接覆盖 State 中的最新快照。
    if (topic == "GameStatus") {
        GameStatus game_status;
        if (game_status.ParseFromString(payload)) {
            log_parse_success(topic, game_status);
            constexpr uint32_t kGameStagePlaying = 4;
            const bool was_playing = state.game_status.has_value()
                                     && state.game_status->current_stage() == kGameStagePlaying;
            const bool is_playing = game_status.current_stage() == kGameStagePlaying;
            if (!was_playing && is_playing) {
                ResetAirSupportCounterForMatchStart(state.air_support_custom_control);
                SPDLOG_INFO("比赛开始，空中支援剩余反制次数初始化为{}",
                            kInitialAirSupportCounterRemaining);
            }
            state.game_status = game_status;
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "GlobalUnitStatus") {
        GlobalUnitStatus global_unit_status;
        if (global_unit_status.ParseFromString(payload)) {
            log_parse_success(topic, global_unit_status);
            state.global_unit_status = global_unit_status;

            // GlobalUnitStatus 同时驱动左右双方血量/弹量面板，并把对方血量打包成
            // CustomControl 下发给机器人电控使用。
            const auto &robot_health = global_unit_status.robot_health();
            const auto update_robot_health = [](RobotStatus &robot, uint32_t health) {
                robot.health = health;
                robot.max_health = std::max(robot.max_health, health);
            };
            bool is_red_team = state.robot_static_status.has_value() && state.robot_static_status->robot_id() < 100;
            auto robot_health_size = robot_health.size();
            auto half_robot_health_size = robot_health_size / 2;

            // 根据 proto V1.3.0 规范，robot_health 数组顺序：
            // 己方 1,2,3,4,7 + 对方 1,2,3,4,7 (共10个)
            // 映射到客户端数组：red_robots/blue_robots[0-8]，其中：
            // 0=英雄(1), 1=工程(2), 2=步兵(3), 3=步兵(4), 4=废弃步兵(5), 5=无人机(6), 6=哨兵(7), 7=飞镖(8), 8=雷达(9)
            // 服务器发送的机器人 ID 顺序：1,2,3,4,7 (映射到客户端索引 0,1,2,3,6)

            // 定义服务器发送的机器人 ID 数组（对应的客户端索引）
            // const int robot_ids[] = {1, 2, 3, 4, 7};  // 服务器发送的机器人 ID
            const int client_indices[] = {0, 1, 2, 3, 6};// 对应的客户端数组索引

            for (int i = 0; i < half_robot_health_size && i < 5; i++) {
                // 根据 proto 注释，robot_health 数组顺序固定为「己方在前，对方在后」
                int self_index = i;
                int enemy_index = i + half_robot_health_size;

                // 映射到客户端数组的正确索引
                int client_index = client_indices[i];
                if (is_red_team) {
                    update_robot_health(state.red_robots[client_index], robot_health[self_index]);
                    update_robot_health(state.blue_robots[client_index], robot_health[enemy_index]);
                } else {
                    update_robot_health(state.blue_robots[client_index], robot_health[self_index]);
                    update_robot_health(state.red_robots[client_index], robot_health[enemy_index]);
                }
            }
            const auto &robot_bullets = global_unit_status.robot_bullets();
            auto robot_bullets_size = robot_bullets.size();
            auto half_robot_bullets_size = robot_bullets_size / 2;
            for (int i = 0; i < half_robot_bullets_size; i++) {
                int index = i;
                // 由于官方没有明确说明顺序，以防万一，这里做一些容错处理
                if (half_robot_bullets_size == 6) {
                    // 实际顺序
                    // 1 2 3 4 6 7
                    if (i >= 4) {
                        index = i + 1;
                    }
                } else if (half_robot_bullets_size == 5) {
                    // 以防万一
                    // 1 3 4 6 7
                    if (i >= 3) {
                        index = i + 2;
                    } else if (i >= 1) {
                        index = i + 1;
                    }
                }
                int red_index;
                int blue_index;
                if (is_red_team) {
                    red_index = i;
                    blue_index = i + half_robot_bullets_size;
                } else {
                    red_index = i + half_robot_bullets_size;
                    blue_index = i;
                }
                if (red_index < robot_bullets_size) {
                    state.red_robots[index].bullets = robot_bullets[red_index];
                }
                if (blue_index < robot_bullets_size) {
                    state.blue_robots[index].bullets = robot_bullets[blue_index];
                }
            }
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "GlobalLogisticsStatus") {
        GlobalLogisticsStatus global_logistics_status;
        if (global_logistics_status.ParseFromString(payload)) {
            log_parse_success(topic, global_logistics_status);
            state.global_logistics_status = global_logistics_status;
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "GlobalSpecialMechanism") {
        GlobalSpecialMechanism global_special_mechanism;
        if (global_special_mechanism.ParseFromString(payload)) {
            log_parse_success(topic, global_special_mechanism);
            state.global_special_mechanism = global_special_mechanism;
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "Event") {
        Event event;
        if (event.ParseFromString(payload)) {
            log_parse_success(topic, event);
            LogEntry log_entry;
            time_t now = time(nullptr);
            tm *timeinfo = localtime(&now);
            strftime(log_entry.time, sizeof(log_entry.time), "%H:%M:%S", timeinfo);
            log_entry.type = 0;
            switch (event.event_id()) {
                case 1: {
                    // 击杀事件（参数 1 为被击杀者，参数 2 为击杀者，如"1,101"，代表蓝方英雄击击毁红方英雄）
                    auto parts = SplitByComma(event.param());
                    if (parts.has_value()) {
                        log_entry.text = GetRobotName(parts->first) + " 击杀了 " + GetRobotName(parts->second);
                    } else {
                        log_entry.text = "击杀事件: " + event.param();
                    }
                    break;
                }
                case 2: {
                    // 前哨站被摧毁事件（参数值为被击毁的目标 id，红方前哨站为 11，蓝方前哨站为 111）
                    const std::string &param = event.param();
                    std::string target_name;
                    if (param == "11") {
                        target_name = "红方前哨站";
                    } else if (param == "111") {
                        target_name = "蓝方前哨站";
                    } else {
                        target_name = "前哨站" + param;
                    }
                    log_entry.text = target_name + "被摧毁";
                    break;
                }
                case 3: {
                    // TODO 暂时不处理
                    return;
                    // 当前大能量机关被成功激活的灯臂数量、平均环数（仅大能量机关被激活时触发该事件，参数值为激活成功臂数，平均环数(浮点数)，比如”10,9.6”代表激活十个灯臂，平均环数为 9.6）
                    auto parts = SplitByComma(event.param());
                    if (parts.has_value()) {
                        log_entry.text = "大能量机关激活成功: " + parts->first + "个灯臂，平均环数" + parts->second;
                    } else {
                        log_entry.text = "大能量机关激活: " + event.param();
                    }
                    break;
                }
                case 4: {
                    // 能量机关进入已激活状态（激活类型 1：小能量机关，2：大能量机关）
                    const std::string &param = event.param();
                    if (param == "1") {
                        log_entry.text = "小能量机关已激活";
                    } else if (param == "2") {
                        log_entry.text = "大能量机关已激活";
                    } else {
                        log_entry.text = "能量机关已激活: " + param;
                    }
                    break;
                }
                case 5: {
                    // TODO 暂时不处理
                    return;
                    // 己方英雄造成狙击伤害（参数值为累计造成狙击伤害数量）
                    log_entry.text = "己方英雄累计狙击伤害: " + event.param();
                    break;
                }
                case 6: {
                    // TODO 暂时不处理
                    return;
                    // 对方英雄造成狙击伤害（参数值为累计造成狙击伤害数量）
                    log_entry.text = "对方英雄累计狙击伤害: " + event.param();
                    break;
                }
                case 7: {
                    // 对方呼叫空中支援（无参数值）
                    NoteOpponentAirSupportCalled(state.air_support_custom_control);
                    log_entry.text = "对方呼叫空中支援";
                    break;
                }
                case 8: {
                    // 对方空中支援被反制（参数值为己方还剩余的可反制次数）
                    const std::string &param = event.param();
                    if (auto remaining = ParseByteParam(param); remaining.has_value()) {
                        if (NoteAirSupportCounterRemaining(state.air_support_custom_control,
                                                           *remaining,
                                                           std::chrono::steady_clock::now())) {
                            SPDLOG_INFO("空中支援反制45s倒计时开始: 己方剩余反制次数={}", *remaining);
                        }
                    }
                    log_entry.text = "对方空中支援被反制，己方剩余反制次数: " + param;
                    break;
                }
                case 9: {
                    // 飞镖命中（第一个参数为命中方，1 为红方命中，2 为蓝方命中；第二个参数值为命中目标）
                    auto parts = SplitByComma(event.param());
                    if (parts.has_value()) {
                        std::string side_name = (parts->first == "1") ? "红方" : "蓝方";
                        const std::string &target = parts->second;

                        std::string target_name;
                        if (target == "1") target_name = "前哨站";
                        else if (target == "2")
                            target_name = "基地固定目标";
                        else if (target == "3")
                            target_name = "基地随机固定目标";
                        else if (target == "4")
                            target_name = "基地随机移动目标";
                        else if (target == "5")
                            target_name = "基地末端移动目标";
                        else
                            target_name = "目标" + target;

                        log_entry.text = side_name + "飞镖命中" + target_name;
                    } else {
                        log_entry.text = "飞镖命中: " + event.param();
                    }
                    break;
                }
                case 10: {
                    // 对方飞镖闸门开启（无参数）
                    log_entry.text = "对方飞镖闸门开启";
                    break;
                }
                case 11: {
                    // TODO 暂时不处理
                    return;
                    // 基地遭到攻击（无参数值，每次触发存在 5s 内置冷却）
                    log_entry.text = "基地遭到攻击";
                    break;
                }
                case 12: {
                    // 对方前哨站停转（无参数）
                    log_entry.text = "对方前哨站停转";
                    break;
                }
                case 13: {
                    // 对方基地护甲展开（无参数）
                    log_entry.text = "对方基地护甲展开";
                    break;
                }
                case 14: {
                    // 装配过程中对方请求四级装配，进入强制退出缓冲期（无参数）
                    log_entry.text = "装配过程中对方请求四级装配，进入强制退出缓冲期";
                    break;
                }
                case 15: {
                    // 装配结果事件(0 成功，1 拔出，2 超时，3 离开装配区过久，4 工程战亡，5 四级难度未满足完成协作时限，6 主动退出，7 完成装配但结算时未检测到有能量单元，8 缓冲期到期，装配流程强制结束)
                    const std::string &param = event.param();
                    std::string result_name;
                    if (param == "0") result_name = "成功";
                    else if (param == "1")
                        result_name = "拔出";
                    else if (param == "2")
                        result_name = "超时";
                    else if (param == "3")
                        result_name = "离开装配区过久";
                    else if (param == "4")
                        result_name = "工程战亡";
                    else if (param == "5")
                        result_name = "四级难度未满足完成协作时限";
                    else if (param == "6")
                        result_name = "主动退出";
                    else if (param == "7")
                        result_name = "完成装配但结算时未检测到有能量单元";
                    else if (param == "8")
                        result_name = "缓冲期到期，装配流程强制结束";
                    else
                        result_name = "未知结果(" + param + ")";
                    log_entry.text = "装配结果: " + result_name;
                    break;
                }
                default: {
                    log_entry.text = "未知事件: ID=" + std::to_string(event.event_id()) + ", 参数=" + event.param();
                    break;
                }
            }
            AppendSystemLog(state, log_entry.type, log_entry.text);
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "RobotInjuryStat") {
        RobotInjuryStat robot_injury_stat;
        if (robot_injury_stat.ParseFromString(payload)) {
            log_parse_success(topic, robot_injury_stat);
            state.robot_injury_stat = robot_injury_stat;
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "RobotRespawnStatus") {
        RobotRespawnStatus robot_respawn_status;
        if (robot_respawn_status.ParseFromString(payload)) {
            log_parse_success(topic, robot_respawn_status);
            state.robot_respawn_status = robot_respawn_status;
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "RobotStaticStatus") {
        RobotStaticStatus robot_static_status;
        if (robot_static_status.ParseFromString(payload)) {
            log_parse_success(topic, robot_static_status);
            state.robot_static_status = robot_static_status;
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "RobotDynamicStatus") {
        RobotDynamicStatus robot_dynamic_status;
        if (robot_dynamic_status.ParseFromString(payload)) {
            log_parse_success(topic, robot_dynamic_status);
            state.robot_dynamic_status = robot_dynamic_status;
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "RobotModuleStatus") {
        RobotModuleStatus robot_module_status;
        if (robot_module_status.ParseFromString(payload)) {
            log_parse_success(topic, robot_module_status);
            state.robot_module_status = robot_module_status;
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "RobotPosition") {
        // TODO 这里文档没有把坐标系没说清楚，需要实际测试。猜测也许与雷达站是一样的？
        RobotPosition robot_position;
        if (robot_position.ParseFromString(payload)) {
            log_parse_success(topic, robot_position);
            state.robot_position = robot_position;
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "Buff") {
        Buff buff;
        if (buff.ParseFromString(payload)) {
            log_parse_success(topic, buff);
            BuffWithTimestamp buff_entry;
            buff_entry.buff = buff;
            buff_entry.update_time = std::chrono::steady_clock::now();
            auto it = std::ranges::find_if(state.active_buffs, [&buff](const BuffWithTimestamp &existing) {
                return existing.buff.robot_id() == buff.robot_id() && existing.buff.buff_type() == buff.buff_type();
            });
            if (it != state.active_buffs.end()) {
                it->buff = buff;
                it->update_time = buff_entry.update_time;
                SPDLOG_INFO("更新Buff robot_id={} buff_type={} left_time={}",
                            buff.robot_id(),
                            buff.buff_type(),
                            buff.buff_left_time());
            } else {
                state.active_buffs.push_back(buff_entry);
                SPDLOG_INFO("新增Buff robot_id={} buff_type={} left_time={}",
                            buff.robot_id(),
                            buff.buff_type(),
                            buff.buff_left_time());
            }
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "PenaltyInfo") {
        PenaltyInfo penalty_info;
        if (penalty_info.ParseFromString(payload)) {
            log_parse_success(topic, penalty_info);
            LogEntry log_entry;
            time_t now = time(nullptr);
            tm *timeinfo = localtime(&now);
            strftime(log_entry.time, sizeof(log_entry.time), "%H:%M:%S", timeinfo);
            log_entry.type = 1;
            uint32_t penalty_type = penalty_info.penalty_type();
            uint32_t penalty_effect_sec = penalty_info.penalty_effect_sec();
            std::string penalty_name;
            switch (penalty_type) {
                case 1:
                    penalty_name = "黄牌";
                    break;
                case 2:
                    penalty_name = "双方黄牌";
                    break;
                case 3:
                    penalty_name = "红牌";
                    break;
                case 4:
                    penalty_name = "超功率";
                    break;
                case 5:
                    penalty_name = "超热量";
                    break;
                case 6:
                    penalty_name = "超射速";
                    break;
                default:
                    penalty_name = "未知判罚类型(" + std::to_string(penalty_type) + ")";
                    break;
            }
            log_entry.text = penalty_name + ", 受罚效果时长" + std::to_string(penalty_effect_sec) + "秒";
            AppendSystemLog(state, log_entry.type, log_entry.text);
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "RobotPathPlanInfo") {
        RobotPathPlanInfo robot_path_plan_info;
        if (robot_path_plan_info.ParseFromString(payload)) {
            log_parse_success(topic, robot_path_plan_info);
            state.robot_path_plan_info = robot_path_plan_info;
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "MapClickInfo") {
        MapClickInfo map_click_info;
        if (map_click_info.ParseFromString(payload)) {
            log_parse_success(topic, map_click_info);
            state.map_click_info = map_click_info;
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "RadarInfoToClient") {
        RadarInfoToClient radar_info_to_client;
        if (radar_info_to_client.ParseFromString(payload)) {
            log_parse_success(topic, radar_info_to_client);
            state.radar_info_to_client = radar_info_to_client;
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "CustomByteBlock") {
        CustomByteBlock custom_byte_block;
        if (custom_byte_block.ParseFromString(payload)) {
            log_parse_success(topic, custom_byte_block);
            const std::string &data = custom_byte_block.data();
            if (!data.empty() && custom_byte_block_packet_handler) {
                custom_byte_block_packet_handler(std::span<const uint8_t>(
                        reinterpret_cast<const uint8_t *>(data.data()),
                        data.size()));
            }
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "TechCoreMotionStateSync") {
        TechCoreMotionStateSync tech_core_motion_state_sync;
        if (tech_core_motion_state_sync.ParseFromString(payload)) {
            log_parse_success(topic, tech_core_motion_state_sync);
            state.tech_core_motion_state_sync = tech_core_motion_state_sync;
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "RobotPerformanceSelectionSync") {
        RobotPerformanceSelectionSync robot_performance_selection_sync;
        if (robot_performance_selection_sync.ParseFromString(payload)) {
            log_parse_success(topic, robot_performance_selection_sync);
            state.robot_performance_selection_sync = robot_performance_selection_sync;
            state.last_received_robot_performance_selection_sync = std::chrono::steady_clock::now();
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "DeployModeStatusSync") {
        DeployModeStatusSync deploy_mode_status_sync;
        if (deploy_mode_status_sync.ParseFromString(payload)) {
            log_parse_success(topic, deploy_mode_status_sync);
            state.deploy_mode_status_sync = deploy_mode_status_sync;
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "RuneStatusSync") {
        RuneStatusSync rune_status_sync;
        if (rune_status_sync.ParseFromString(payload)) {
            log_parse_success(topic, rune_status_sync);
            state.rune_status_sync = rune_status_sync;
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "SentryStatusSync") {
        SentryStatusSync sentry_status_sync;
        if (sentry_status_sync.ParseFromString(payload)) {
            log_parse_success(topic, sentry_status_sync);
            state.sentry_status_sync = sentry_status_sync;
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "DartSelectTargetStatusSync") {
        DartSelectTargetStatusSync dart_select_target_status_sync;
        if (dart_select_target_status_sync.ParseFromString(payload)) {
            log_parse_success(topic, dart_select_target_status_sync);
            state.dart_select_target_status_sync = dart_select_target_status_sync;
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "SentryCtrlResult") {
        SentryCtrlResult sentry_ctrl_result;
        if (sentry_ctrl_result.ParseFromString(payload)) {
            log_parse_success(topic, sentry_ctrl_result);
            // TODO 需要添加相关UI
        } else {
            log_parse_failure(topic);
        }
    } else if (topic == "AirSupportStatusSync") {
        AirSupportStatusSync air_support_status_sync;
        if (air_support_status_sync.ParseFromString(payload)) {
            log_parse_success(topic, air_support_status_sync);
            auto previous = state.air_support_status_sync;
            state.air_support_status_sync = air_support_status_sync;

            if (!previous.has_value()) {
                if (air_support_status_sync.airsupport_status() == 1) {
                    AppendSystemLog(state, 0, "空中支援已开始");
                }
            } else if (previous->airsupport_status() != air_support_status_sync.airsupport_status()) {
                if (air_support_status_sync.airsupport_status() == 1) {
                    AppendSystemLog(state, 0, "空中支援已开始");
                } else {
                    AppendSystemLog(state, 0, "空中支援已结束");
                }
            }

            if (!previous.has_value() || previous->is_being_targeted() != air_support_status_sync.is_being_targeted()) {
                if (air_support_status_sync.is_being_targeted() != 0) {
                    AppendSystemLog(state, 1, "空中机器人正在被激光照射");
                } else if (previous.has_value()) {
                    AppendSystemLog(state, 0, "空中机器人已脱离激光照射");
                }
            }

            if (!previous.has_value() || previous->shooter_status() != air_support_status_sync.shooter_status()) {
                if (air_support_status_sync.shooter_status() == 0) {
                    AppendSystemLog(state, 1, "空中机器人发射机构被锁定，无法解除");
                } else if (air_support_status_sync.shooter_status() == 2) {
                    AppendSystemLog(state, 1, "空中机器人发射机构被锁定，可花费金币解除");
                } else if (previous.has_value()) {
                    AppendSystemLog(state, 0, "空中机器人发射机构恢复正常");
                }
            }
        } else {
            log_parse_failure(topic);
        }
    } else {
        SPDLOG_WARN("收到未处理MQTT话题 topic={} bytes={}", topic, payload.size());
    }
}

std::string GetRobotName(const std::string &robot_id) {
    if (robot_id.empty()) {
        SPDLOG_WARN("收到空机器人ID");
        return "未知机器人";
    }

    int id = 0;
    try {
        id = std::stoi(robot_id);
    } catch (const std::exception &error) {
        SPDLOG_WARN("机器人ID解析失败 robot_id={} message={}", robot_id, error.what());
        return "未知机器人";
    }
    bool is_red = id < 100;
    int num = is_red ? id : id - 100;

    std::string prefix = is_red ? "红方" : "蓝方";
    std::string suffix;

    switch (num) {
        case 1:
            suffix = "英雄";
            break;
        case 2:
            suffix = "工程";
            break;
        case 3:
            suffix = "步兵3";
            break;
        case 4:
            suffix = "步兵4";
            break;
        case 5:
            suffix = "步兵5";
            break;
        case 6:
            suffix = "无人机";
            break;
        case 7:
            suffix = "哨兵";
            break;
        case 8:
            suffix = "飞镖";
            break;
        case 9:
            suffix = "雷达";
            break;
        case 11:
            suffix = "前哨站";
            break;
        case 12:
            suffix = "基地";
            break;
        default:
            suffix = std::to_string(num) + "号";
            break;
    }

    return prefix + suffix;
}

const char *GetAirSupportStatusName(uint32_t airsupport_status) {
    switch (airsupport_status) {
        case 0:
            return "未进行空中支援";
        case 1:
            return "正在空中支援";
        default:
            return "未知空中支援状态";
    }
}

const char *GetAirSupportShooterStatusName(uint32_t shooter_status) {
    switch (shooter_status) {
        case 0:
            return "已锁定(不可解除)";
        case 1:
            return "正常";
        case 2:
            return "已锁定(可解除)";
        default:
            return "未知";
    }
}
