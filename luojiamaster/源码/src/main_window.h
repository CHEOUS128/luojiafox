#pragma once

#include <pch.h>
#include "custom_control_payload.h"
#include "io/mqtt_client.h"

struct LogEntry {
    char time[9];
    std::string text;
    int type;// 0: INFO, 1: WARNING, 2: ERROR
    uint64_t sequence = 0;
};

struct RobotStatus {
    uint32_t health = 0;
    uint32_t max_health = 0;
    int32_t bullets = 0;
};

struct MapPosition {
    int id;
    float x;
    float y;
};

struct BuffWithTimestamp {
    Buff buff;
    std::chrono::steady_clock::time_point update_time;
};

struct State {
    std::optional<GameStatus> game_status;
    std::optional<GlobalUnitStatus> global_unit_status;
    std::optional<GlobalLogisticsStatus> global_logistics_status;
    std::optional<GlobalSpecialMechanism> global_special_mechanism;
    std::optional<RobotInjuryStat> robot_injury_stat;
    std::optional<RobotRespawnStatus> robot_respawn_status;
    std::optional<RobotStaticStatus> robot_static_status;
    std::optional<RobotDynamicStatus> robot_dynamic_status;
    std::optional<RobotModuleStatus> robot_module_status;
    std::optional<RobotPosition> robot_position;
    std::vector<BuffWithTimestamp> active_buffs;
    std::optional<RobotPathPlanInfo> robot_path_plan_info;
    std::optional<MapClickInfo> map_click_info;
    std::optional<RadarInfoToClient> radar_info_to_client;
    std::optional<TechCoreMotionStateSync> tech_core_motion_state_sync;
    std::optional<RobotPerformanceSelectionSync> robot_performance_selection_sync;
    std::chrono::steady_clock::time_point last_received_robot_performance_selection_sync;
    std::optional<DeployModeStatusSync> deploy_mode_status_sync;
    std::optional<RuneStatusSync> rune_status_sync;
    std::optional<SentryStatusSync> sentry_status_sync;
    std::optional<DartSelectTargetStatusSync> dart_select_target_status_sync;
    std::optional<SentryCtrlResult> sentry_ctrl_result;
    std::optional<AirSupportStatusSync> air_support_status_sync;
    AirSupportCustomControlState air_support_custom_control;
    std::optional<std::chrono::steady_clock::time_point> last_custom_control_publish_time;

    // 自定义状态
    bool auto_aim = false;
    bool gyro = false;

    // 机器人状态：英雄，工程，步兵x3，无人机，哨兵，飞镖，雷达
    RobotStatus red_robots[9];
    RobotStatus blue_robots[9];

    // 日志
    uint64_t next_log_sequence = 1;
    std::vector<LogEntry> vehicle_logs;
    std::vector<LogEntry> system_logs;
};

using CustomByteBlockPacketHandler = std::function<void(std::span<const uint8_t> packet_bytes)>;

inline constexpr auto kUdpVideoFallbackTimeout = std::chrono::milliseconds(250);

// 「主画面偏好 = 自定义流」时的两档超时。
// 自定义流是 4.8 帧/秒、且在比赛链路里会成段丢包（实测：有键鼠输入的时段里
// 自定义流也只有 44% 的时间「可显示」，断口中位数 2.9 秒）。按 1 秒判死会让
// 主画面在 UDP 和自定义流之间来回抢，官方图传跟着一闪一闪。
//   - 双画面模式：超过 kCustomVideoLiveTimeout 没新帧就只把这一格标成不可用；
//   - 主画面模式：超过 kCustomVideoFallbackTimeout 才把主画面让给 UDP。
inline constexpr auto kCustomVideoLiveTimeout = std::chrono::milliseconds(1000);
inline constexpr auto kCustomVideoFallbackTimeout = std::chrono::milliseconds(5000);

State CreateInitialState();

bool ShowMainWindow(const State &state,
                    RoboGo::io::MqttClient &mqtt_client,
                    ImTextureRef video_texture_ref,
                    const ImVec2 &video_size,
                    ImTextureRef custom_video_texture_ref,
                    const ImVec2 &custom_video_size,
                    bool using_custom_video_as_main,
                    bool prefer_custom_video_as_main,
                    bool udp_video_display_ready,
                    bool custom_video_display_ready,
                    bool custom_video_hold_as_main,
                    bool udp_video_inverted,
                    ImTextureRef mini_map_texture_ref,
                    const ImVec2 &mini_map_texture_size);

void UpdateState(State &state);

void PublishCustomControlIfDue(State &state, RoboGo::io::MqttClient &mqtt_client);

// 处理从 MQTT 收到的裁判系统/模拟器消息：
// 按 topic 解析对应 protobuf，刷新 UI 状态，必要时生成系统日志或透传自定义字节块。
void HandleMqttMessage(State &state,
                       RoboGo::io::MqttClient &mqtt_client,
                       const std::string &topic,
                       const std::string &payload,
                       const CustomByteBlockPacketHandler &custom_byte_block_packet_handler = {});

void AppendVehicleLog(State &state, int type, const std::string &text);

void AppendSystemLog(State &state, int type, const std::string &text);

std::string GetRobotName(const std::string &robot_id);

const char *GetAirSupportStatusName(uint32_t airsupport_status);

const char *GetAirSupportShooterStatusName(uint32_t shooter_status);
