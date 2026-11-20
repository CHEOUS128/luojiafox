#include "start_panel.h"
#include "panel/brand_mark.h"
#include "keyboard_mouse_state_manager.h"
#include "logging/logging.h"
#include "panel_layout.h"
#include <span>

namespace {

using RoboGo::panel::ButtonStyle;
using RoboGo::panel::DrawPanelTitle;
using RoboGo::panel::MakeButtonStyle;
using RoboGo::panel::MakeInputStyle;
using RoboGo::panel::MakeMouseSensitivitySliderStyle;
using RoboGo::panel::MakeTextColorStyle;
using RoboGo::panel::MakeWindowStyle;
using RoboGo::panel::ScopedChildWindow;
using RoboGo::panel::ScopedStyle;
using RoboGo::panel::ScopedTitledPanel;
using RoboGo::panel::ShowPanelRow;

constexpr std::array<const char *, 2> kLoginTeamNames = {"红队", "蓝队"};
constexpr std::array<const char *, 5> kLoginRobotNames = {"英雄", "工程", "3号步兵", "4号步兵", "云台手"};
constexpr float kLoginStatusGap = 4.0f;
constexpr float kLoginSectionGap = 8.0f;
constexpr float kLoginTargetGap = 10.0f;
constexpr float kLoginMqttInputTopMargin = 4.0f;
constexpr float kPrimaryActionButtonHeight = 30.0f;
constexpr float kLoginTeamButtonHeight = kPrimaryActionButtonHeight;
constexpr float kLoginRobotButtonHeight = kPrimaryActionButtonHeight;
constexpr float kMouseSensitivityTopGap = 8.0f;
constexpr float kLoginPanelBottomPadding = 12.0f;
constexpr std::string_view kDefaultMqttPort = "3333";

constexpr std::array<std::array<int, 5>, 2> kLoginRobotIds = {{
        {1, 2, 3, 4, 6},
        {101, 102, 103, 104, 106},
}};

constexpr std::array<const char *, 5> kDartTargetNames = {
        "前哨站",
        "基地固定目标",
        "基地随机固定目标",
        "基地随机移动目标",
        "基地末端移动目标",
};

constexpr uint32_t kAmmoExchangeBuffType = 6;
constexpr auto kPerformanceSelectionRemoteSyncIgnoreDuration = std::chrono::seconds(5);

template<typename Message>
void PublishProto(RoboGo::io::MqttClient &mqtt_client, const char *topic, const Message &message, bool enter_game_mode_after_send = false) {
    std::string serialized;
    if (message.SerializeToString(&serialized)) {
        SPDLOG_INFO("发布控制消息 topic={} bytes={} enter_game_mode_after_send={}",
                    topic,
                    serialized.size(),
                    enter_game_mode_after_send);
        mqtt_client.publish(topic, serialized);
        if (enter_game_mode_after_send) {
            enter_game_mode();
        }
    } else {
        SPDLOG_ERROR("控制消息序列化失败 topic={}", topic);
    }
}

uint32_t GetCurrentRobotId(const State &state) {
    return state.robot_static_status.has_value() ? state.robot_static_status->robot_id() : 0;
}

template<typename... Ids>
bool IsOneOf(uint32_t robot_id, Ids... ids) {
    return ((robot_id == static_cast<uint32_t>(ids)) || ...);
}

bool HasActiveBuff(const State &state, uint32_t buff_type) {
    uint32_t current_robot_id = GetCurrentRobotId(state);
    if (current_robot_id == 0) {
        return false;
    }
    return std::ranges::any_of(state.active_buffs, [current_robot_id, buff_type](const BuffWithTimestamp &buff_entry) {
        return buff_entry.buff.robot_id() == current_robot_id && buff_entry.buff.buff_type() == buff_type;
    });
}

void IntegerInputText(const char *label, std::string *str, bool *is_valid, long long *value) {
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", label);
    ImGui::SameLine();
    ImGui::InputText("", str, ImGuiInputTextFlags_CharsDecimal);
    char *endPtr;
    *value = std::strtoll(str->c_str(), &endPtr, 10);
    *is_valid = *endPtr == '\0';
}

bool ButtonWithTooltip(const char *label, const std::string &valid_tooltip, const char *disable_reason) {
    ImGui::BeginDisabled(disable_reason != nullptr);
    bool is_clicked = ImGui::Button(label);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        if (disable_reason != nullptr) {
            ImGui::SetTooltip("%s", disable_reason);
        } else if (!valid_tooltip.empty()) {
            ImGui::SetTooltip("%s", valid_tooltip.c_str());
        }
    }
    return is_clicked;
}

void SendAirSupportCommand(RoboGo::io::MqttClient &mqtt_client, uint32_t command_id) {
    AirSupportCommand air_support_command;
    air_support_command.set_command_id(command_id);
    PublishProto(mqtt_client, "AirSupportCommand", air_support_command, true);
}

struct StartPanelVisibility {
    bool show_ammo_panel = false;
    bool show_recover_panel = false;
    bool show_revive_panel = false;
    bool show_deploy_panel = false;
    bool show_assembly_panel = false;
    bool show_marker_panel = false;
    bool show_performance_panel = false;
    bool show_dart_panel = false;
    bool show_air_support_panel = false;
    bool is_hero = false;
    bool show_robot_performance_options = false;
    bool show_sentry_performance_option = false;
};

struct ButtonFeedback {
    std::string valid_tooltip;
    const char *disabled_reason = nullptr;
};

struct TeamRobotSelectionState {
    int selected_team = 0;
    int selected_robot = 0;
    bool commander = false;
};

struct MqttServerAddressState {
    std::string input = "127.0.0.1:3333";
    std::string error_message;
    bool initialized = false;
};

struct AmmoPanelState {
    std::string ammo_str = "10";
};

struct AssemblyPanelState {
    std::string difficulty_str = "3";
};

struct DartPanelState {
    uint32_t target_id = 1;
    bool open = false;
};

struct PerformancePanelState {
    uint32_t shooter = std::numeric_limits<uint32_t>::max();
    uint32_t chassis = std::numeric_limits<uint32_t>::max();
    uint32_t sentry_control = std::numeric_limits<uint32_t>::max();
    std::chrono::steady_clock::time_point last_handled_remote_sync = std::chrono::steady_clock::time_point::min();
    std::chrono::steady_clock::time_point ignore_remote_sync_until = std::chrono::steady_clock::time_point::min();
    struct Choice {
        std::string label;
        uint32_t value = 0;
        std::chrono::steady_clock::time_point selected_at;
    };
    std::array<std::optional<Choice>, 3> requested;
    uint64_t round_generation = 0;
};

struct MenuInputs {
    TeamRobotSelectionState selection;
    MqttServerAddressState endpoint;
    AmmoPanelState ammo;
    AssemblyPanelState assembly;
    DartPanelState dart;
    PerformancePanelState performance;
};
MenuInputs menu_inputs;

using RoboGo::app::Competition;
const char *RoleLabel(int index, Competition competition) {
    if (competition != Competition::Uc2026) {
        if (index == 0) return "重装 1 号";
        if (index == 2) return "步兵 3 号";
        if (index == 3) return "步兵 4 号";
    }
    return kLoginRobotNames[index];
}

const ButtonStyle kPrimaryButtonStyle = {
        ImVec4(0.2f, 0.6f, 1.0f, 0.8f),
        ImVec4(0.3f, 0.7f, 1.0f, 1.0f),
        ImVec4(0.1f, 0.5f, 0.9f, 1.0f),
};

const ButtonStyle kCompactOptionButtonStyle = {
        ImVec4(0.16f, 0.2f, 0.26f, 0.95f),
        ImVec4(0.2f, 0.24f, 0.31f, 1.0f),
        ImVec4(0.12f, 0.16f, 0.22f, 1.0f),
        ImVec2(10.0f, 6.0f),
        7.0f,
};

const ButtonStyle kSelectedRedRobotButtonStyle = {
        ImVec4(0.84f, 0.26f, 0.26f, 0.9f),
        ImVec4(0.92f, 0.34f, 0.34f, 1.0f),
        ImVec4(0.74f, 0.18f, 0.18f, 1.0f),
        ImVec2(10.0f, 6.0f),
        7.0f,
};

const ButtonStyle kSelectedBlueRobotButtonStyle = {
        ImVec4(0.2f, 0.6f, 1.0f, 0.9f),
        ImVec4(0.28f, 0.68f, 1.0f, 1.0f),
        ImVec4(0.14f, 0.5f, 0.92f, 1.0f),
        ImVec2(10.0f, 6.0f),
        7.0f,
};

const ButtonStyle kSelectedRedTeamButtonStyle = {
        ImVec4(0.88f, 0.28f, 0.28f, 0.92f),
        ImVec4(0.95f, 0.35f, 0.35f, 1.0f),
        ImVec4(0.78f, 0.2f, 0.2f, 1.0f),
        ImVec2(10.0f, 6.0f),
        7.0f,
};

const ButtonStyle kSelectedBlueTeamButtonStyle = {
        ImVec4(0.18f, 0.52f, 0.98f, 0.92f),
        ImVec4(0.26f, 0.6f, 1.0f, 1.0f),
        ImVec4(0.12f, 0.42f, 0.9f, 1.0f),
        ImVec2(10.0f, 6.0f),
        7.0f,
};

const ButtonStyle kAssemblyButtonStyle = {
        ImVec4(0.3f, 0.8f, 1.0f, 0.8f),
        ImVec4(0.4f, 0.9f, 1.0f, 1.0f),
        ImVec4(0.2f, 0.7f, 0.9f, 1.0f),
};

const ButtonStyle kDeployButtonStyle = {
        ImVec4(0.7f, 0.3f, 1.0f, 0.8f),
        ImVec4(0.8f, 0.4f, 1.0f, 1.0f),
        ImVec4(0.6f, 0.2f, 0.9f, 1.0f),
};

const ButtonStyle kRecoverButtonStyle = {
        ImVec4(1.0f, 0.3f, 0.3f, 0.8f),
        ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
        ImVec4(0.9f, 0.2f, 0.2f, 1.0f),
};

const ButtonStyle kReviveButtonStyle = {
        ImVec4(1.0f, 1.0f, 0.3f, 0.8f),
        ImVec4(1.0f, 1.0f, 0.4f, 1.0f),
        ImVec4(0.9f, 0.9f, 0.2f, 1.0f),
};

const ButtonStyle kMarkerButtonStyle = {
        ImVec4(1.0f, 0.6f, 0.2f, 0.8f),
        ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
        ImVec4(0.9f, 0.5f, 0.1f, 1.0f),
};

const ButtonStyle kDartButtonStyle = {
        ImVec4(1.0f, 0.4f, 0.8f, 0.8f),
        ImVec4(1.0f, 0.5f, 0.9f, 1.0f),
        ImVec4(0.9f, 0.3f, 0.7f, 1.0f),
};

const ButtonStyle kAirSupportButtonStyle = {
        ImVec4(0.2f, 0.85f, 1.0f, 0.8f),
        ImVec4(0.3f, 0.9f, 1.0f, 1.0f),
        ImVec4(0.15f, 0.7f, 0.9f, 1.0f),
};

const ButtonStyle kEnterGameButtonStyle = {
        ImVec4(0.0f, 0.8f, 0.4f, 0.9f),
        ImVec4(0.0f, 1.0f, 0.5f, 1.0f),
        ImVec4(0.0f, 0.6f, 0.3f, 1.0f),
        ImVec2(0.0f, 0.0f),
        8.0f,
};

ButtonFeedback MakeDisabledFeedback(const char *reason) {
    ButtonFeedback feedback;
    feedback.disabled_reason = reason;
    return feedback;
}

ButtonFeedback MakePriceFeedback(uint32_t price) {
    ButtonFeedback feedback;
    feedback.valid_tooltip = "将消耗" + std::to_string(price) + "金币";
    return feedback;
}

ButtonFeedback GetEconomyFeedback(const State &state, uint32_t price) {
    if (!state.global_logistics_status.has_value()) {
        return MakeDisabledFeedback("无法获取当前经济");
    }
    if (state.global_logistics_status->remaining_economy() < price) {
        return MakeDisabledFeedback("当前经济不足");
    }
    return MakePriceFeedback(price);
}

ButtonFeedback GetAmmoExchangeFeedback(const State &state, bool is_valid_ammo, long long ammo, bool is_hero) {
    if (!HasActiveBuff(state, kAmmoExchangeBuffType)) {
        return MakeDisabledFeedback("不在补给区内");
    }
    if (!is_valid_ammo) {
        return MakeDisabledFeedback("用户输入存在非法字符");
    }
    if (is_hero) {
        return GetEconomyFeedback(state, static_cast<uint32_t>(ammo * 10));
    }
    if (ammo % 10 != 0) {
        return MakeDisabledFeedback("兑换的允许发弹量必须是 10 的倍数");
    }
    return GetEconomyFeedback(state, static_cast<uint32_t>(ammo));
}

ButtonFeedback GetRemoteAmmoExchangeFeedback(const State &state, bool is_valid_ammo, long long ammo, bool is_hero) {
    if (!is_valid_ammo) {
        return MakeDisabledFeedback("用户输入存在非法字符");
    }
    if (is_hero) {
        if (ammo % 10 != 0) {
            return MakeDisabledFeedback("兑换的允许发弹量必须是 10 的倍数");
        }
        return GetEconomyFeedback(state, static_cast<uint32_t>(ammo * 15));
    }
    if (ammo % 100 != 0) {
        return MakeDisabledFeedback("兑换的允许发弹量必须是 100 的倍数");
    }
    return GetEconomyFeedback(state, static_cast<uint32_t>(ammo * 1.5));
}

ButtonFeedback GetRecoverFeedback(const State &state) {
    if (!state.game_status.has_value()) {
        return MakeDisabledFeedback("无法获取比赛状态");
    }
    uint32_t price = static_cast<uint32_t>(50 + std::ceil(420 - state.game_status->stage_countdown_sec() / 60 * 20));
    return GetEconomyFeedback(state, price);
}

ButtonFeedback GetConfirmReviveFeedback(const State &state) {
    if (!state.robot_respawn_status.has_value()) {
        return MakeDisabledFeedback("无法获取复活状态");
    }
    const auto &respawn_status = *state.robot_respawn_status;
    if (!respawn_status.is_pending_respawn()) {
        return MakeDisabledFeedback("机器人不在复活状态");
    }
    if (respawn_status.total_respawn_progress() > 0 &&
        respawn_status.current_respawn_progress() < respawn_status.total_respawn_progress()) {
        return MakeDisabledFeedback("复活读条未完成");
    }
    if (!respawn_status.can_free_respawn()) {
        return MakeDisabledFeedback("无法免费复活");
    }
    return {};
}

ButtonFeedback GetInstantReviveFeedback(const State &state) {
    if (!state.robot_respawn_status.has_value()) {
        return MakeDisabledFeedback("无法获取复活状态");
    }
    const auto &respawn_status = *state.robot_respawn_status;
    if (!respawn_status.can_pay_for_respawn()) {
        return MakeDisabledFeedback("无法花费金币复活");
    }
    uint32_t price = respawn_status.gold_cost_for_respawn();
    return GetEconomyFeedback(state, price);
}

ButtonFeedback GetDartLaunchFeedback(const State &state) {
    if (!state.dart_select_target_status_sync.has_value()) {
        return MakeDisabledFeedback("无法获取飞镖状态");
    }
    if (state.dart_select_target_status_sync->open() != 2) {
        return MakeDisabledFeedback("闸门未开启");
    }
    return {};
}

ButtonFeedback GetFreeAirSupportFeedback(const State &state) {
    if (!state.air_support_status_sync.has_value()) {
        return MakeDisabledFeedback("无法获取空中支援状态");
    }
    const auto &sync = *state.air_support_status_sync;
    if (sync.airsupport_status() == 1) {
        return MakeDisabledFeedback("当前正在空中支援");
    }
    if (sync.left_time() == 0) {
        return MakeDisabledFeedback("当前没有可用的免费空中支援时长");
    }
    return {};
}

ButtonFeedback GetPaidAirSupportFeedback(const State &state) {
    if (!state.air_support_status_sync.has_value()) {
        return MakeDisabledFeedback("无法获取空中支援状态");
    }
    const auto &sync = *state.air_support_status_sync;
    if (sync.airsupport_status() == 1) {
        return MakeDisabledFeedback("当前正在空中支援");
    }

    ButtonFeedback feedback;
    feedback.valid_tooltip = sync.left_time() > 0 ? "若仍有免费时长，将优先使用免费时长" : "发起付费空中支援";
    return feedback;
}

ButtonFeedback GetCancelAirSupportFeedback(const State &state) {
    if (!state.air_support_status_sync.has_value()) {
        return MakeDisabledFeedback("无法获取空中支援状态");
    }
    if (state.air_support_status_sync->airsupport_status() != 1) {
        return MakeDisabledFeedback("当前未进行空中支援");
    }
    return {};
}

ButtonFeedback GetReleaseAirSupportCountermeasureFeedback(const State &state) {
    if (!state.air_support_status_sync.has_value()) {
        return MakeDisabledFeedback("无法获取空中支援状态");
    }
    if (state.air_support_status_sync->shooter_status() != 2) {
        return MakeDisabledFeedback("当前发射机构不可解除反制");
    }
    ButtonFeedback feedback;
    feedback.valid_tooltip = "花费金币解除空中机器人发射机构反制";
    return feedback;
}

ScopedStyle MakeEnterGameButtonStyle() {
    ScopedStyle style = MakeButtonStyle(kEnterGameButtonStyle);
    style.PushVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
    style.PushColor(ImGuiCol_Border, ImVec4(0.3f, 1.0f, 0.6f, 0.8f));
    style.PushColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    return style;
}

ScopedStyle MakeStartPanelWindowStyle() {
    return MakeWindowStyle(ImVec4(0.06f, 0.11f, 0.15f, 0.84f), 8.0f, ImVec2(15.0f, 15.0f));
}

const char *GetLoginConnectionStateText(RoboGo::io::MqttClient::ConnectionState connection_state) {
    switch (connection_state) {
        case RoboGo::io::MqttClient::ConnectionState::Idle:
            return "未登录";
        case RoboGo::io::MqttClient::ConnectionState::Connecting:
            return "连接中";
        case RoboGo::io::MqttClient::ConnectionState::Connected:
            return "已登录";
        case RoboGo::io::MqttClient::ConnectionState::Failed:
            return "连接失败，重试中";
        default:
            return "未知状态";
    }
}

ImVec4 GetLoginConnectionStateColor(RoboGo::io::MqttClient::ConnectionState connection_state) {
    switch (connection_state) {
        case RoboGo::io::MqttClient::ConnectionState::Idle:
            return ImVec4(0.7f, 0.7f, 0.75f, 1.0f);
        case RoboGo::io::MqttClient::ConnectionState::Connecting:
            return ImVec4(1.0f, 0.8f, 0.35f, 1.0f);
        case RoboGo::io::MqttClient::ConnectionState::Connected:
            return ImVec4(0.35f, 1.0f, 0.55f, 1.0f);
        case RoboGo::io::MqttClient::ConnectionState::Failed:
            return ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
        default:
            return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    }
}

std::string GetSelectedLoginTargetText(const TeamRobotSelectionState &selection_state, Competition competition) {
    if (selection_state.commander)
        return std::string(kLoginTeamNames[selection_state.selected_team]) + " / 指挥手";
    const int robot_id = kLoginRobotIds[selection_state.selected_team][selection_state.selected_robot];
    return std::string(kLoginTeamNames[selection_state.selected_team]) + " / " +
           RoleLabel(selection_state.selected_robot, competition) + " (ID " + std::to_string(robot_id) + ")";
}

std::string TrimAsciiWhitespace(std::string_view text) {
    auto is_space = [](char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };

    std::size_t start = 0;
    while (start < text.size() && is_space(text[start])) {
        start += 1;
    }
    std::size_t end = text.size();
    while (end > start && is_space(text[end - 1])) {
        end -= 1;
    }
    return std::string(text.substr(start, end - start));
}

bool ParseMqttServerEndpoint(const std::string &input, std::string &host, std::string &port, std::string &error_message) {
    std::string normalized = TrimAsciiWhitespace(input);
    if (normalized.empty()) {
        error_message = "请输入 MQTT 服务端地址";
        return false;
    }
    if (normalized.starts_with("tcp://")) {
        normalized = normalized.substr(6);
    }
    normalized = TrimAsciiWhitespace(normalized);
    if (normalized.empty()) {
        error_message = "服务端地址不能为空";
        return false;
    }

    std::string host_part;
    std::string port_part;
    if (normalized.front() == '[') {
        const std::size_t close_bracket = normalized.find(']');
        if (close_bracket == std::string::npos) {
            error_message = "IPv6 地址请使用 [地址]:端口 格式";
            return false;
        }
        host_part = normalized.substr(1, close_bracket - 1);
        if (close_bracket + 1 < normalized.size()) {
            if (normalized[close_bracket + 1] != ':') {
                error_message = "IPv6 地址后只能跟端口";
                return false;
            }
            port_part = normalized.substr(close_bracket + 2);
        }
    } else {
        const std::size_t first_colon = normalized.find(':');
        const std::size_t last_colon = normalized.rfind(':');
        if (first_colon == std::string::npos) {
            host_part = normalized;
        } else if (first_colon == last_colon) {
            host_part = normalized.substr(0, first_colon);
            port_part = normalized.substr(first_colon + 1);
        } else {
            error_message = "检测到多个冒号，请使用 [IPv6]:端口 格式";
            return false;
        }
    }

    host_part = TrimAsciiWhitespace(host_part);
    port_part = TrimAsciiWhitespace(port_part);
    if (host_part.empty()) {
        error_message = "服务端主机不能为空";
        return false;
    }
    if (port_part.empty()) {
        port_part = std::string(kDefaultMqttPort);
    }
    if (!std::ranges::all_of(port_part, [](char ch) { return ch >= '0' && ch <= '9'; })) {
        error_message = "端口必须为数字";
        return false;
    }

    const auto parsed_port = std::strtoll(port_part.c_str(), nullptr, 10);
    if (parsed_port <= 0 || parsed_port > 65535) {
        error_message = "端口范围应为 1 - 65535";
        return false;
    }

    host = host_part;
    port = std::to_string(parsed_port);
    error_message.clear();
    return true;
}

TeamRobotSelectionState &GetTeamRobotSelectionState() {
    return menu_inputs.selection;
}

MqttServerAddressState &GetMqttServerAddressState(RoboGo::io::MqttClient &mqtt_client) {
    auto &mqtt_server_state = menu_inputs.endpoint;
    if (!mqtt_server_state.initialized) {
        const auto [host, port] = mqtt_client.get_server_endpoint();
        if (!host.empty() && !port.empty()) {
            mqtt_server_state.input = host + ":" + port;
        }
        mqtt_server_state.initialized = true;
    }
    return mqtt_server_state;
}

AmmoPanelState &GetAmmoPanelState() {
    return menu_inputs.ammo;
}

AssemblyPanelState &GetAssemblyPanelState() {
    return menu_inputs.assembly;
}

DartPanelState &GetDartPanelState(const State &state) {
    auto &dart_panel_state = menu_inputs.dart;
    if (state.dart_select_target_status_sync.has_value()) {
        if (dart_panel_state.target_id != state.dart_select_target_status_sync->target_id() ||
            dart_panel_state.open != (state.dart_select_target_status_sync->open() != 0)) {
            SPDLOG_DEBUG("同步飞镖状态 target_id={} open={}",
                         state.dart_select_target_status_sync->target_id(),
                         state.dart_select_target_status_sync->open() != 0);
        }
        dart_panel_state.target_id = state.dart_select_target_status_sync->target_id();
        dart_panel_state.open = state.dart_select_target_status_sync->open() != 0;
    }
    return dart_panel_state;
}

PerformancePanelState &GetPerformancePanelState(const State &state) {
    auto &performance_panel_state = menu_inputs.performance;
    if (state.competition == RoboGo::app::Competition::Ul2027 &&
        performance_panel_state.round_generation != state.round_generation) {
        performance_panel_state = {};
        performance_panel_state.round_generation = state.round_generation;
    }
    if (state.robot_performance_selection_sync.has_value() &&
        performance_panel_state.last_handled_remote_sync < state.last_received_robot_performance_selection_sync) {
        performance_panel_state.last_handled_remote_sync = state.last_received_robot_performance_selection_sync;
        if (state.last_received_robot_performance_selection_sync < performance_panel_state.ignore_remote_sync_until) {
            SPDLOG_DEBUG("忽略性能选择远端同步，等待本地下发结果稳定");
        } else {
            const auto &sync = *state.robot_performance_selection_sync;
            const bool partial = state.competition == RoboGo::app::Competition::Ul2027;
            if (!partial || sync.has_shooter()) performance_panel_state.shooter = sync.shooter();
            if (!partial || sync.has_chassis()) performance_panel_state.chassis = sync.chassis();
            if (!partial || sync.has_sentry_control()) performance_panel_state.sentry_control = sync.sentry_control();
        }
    }
    return performance_panel_state;
}

void SendCommonCommand(RoboGo::io::MqttClient &mqtt_client, uint32_t cmd_type, uint32_t param = 0) {
    CommonCommand common_command;
    common_command.set_cmd_type(cmd_type);
    common_command.set_param(param);
    PublishProto(mqtt_client, "CommonCommand", common_command, true);
}

void SendDartCommand(RoboGo::io::MqttClient &mqtt_client, uint32_t target_id, bool open, bool launch_confirm, bool enter_game_mode_after_send = false) {
    DartCommand dart_command;
    dart_command.set_target_id(target_id);
    dart_command.set_open(open);
    dart_command.set_launch_confirm(launch_confirm);
    PublishProto(mqtt_client, "DartCommand", dart_command, enter_game_mode_after_send);
}

void SendAssemblyCommand(RoboGo::io::MqttClient &mqtt_client, uint32_t difficulty, uint32_t operation) {
    AssemblyCommand assembly_command;
    assembly_command.set_difficulty(difficulty);
    assembly_command.set_operation(operation);
    PublishProto(mqtt_client, "AssemblyCommand", assembly_command, true);
}

void SendDeployModeCommand(RoboGo::io::MqttClient &mqtt_client, uint32_t mode) {
    HeroDeployModeEventCommand hero_deploy_mode_event_command;
    hero_deploy_mode_event_command.set_mode(mode);
    PublishProto(mqtt_client, "HeroDeployModeEventCommand", hero_deploy_mode_event_command, true);
}

void SendPerformanceSelection(RoboGo::io::MqttClient &mqtt_client, PerformancePanelState &performance_state) {
    performance_state.ignore_remote_sync_until = std::chrono::steady_clock::now() + kPerformanceSelectionRemoteSyncIgnoreDuration;
    RobotPerformanceSelectionCommand performance_command;
    performance_command.set_shooter(performance_state.shooter);
    performance_command.set_chassis(performance_state.chassis);
    performance_command.set_sentry_control(performance_state.sentry_control);
    PublishProto(mqtt_client, "RobotPerformanceSelectionCommand", performance_command);
}

bool ShowLabeledRadioPair(const char *label,
                          float first_option_x,
                          float second_option_x,
                          const char *first_option_label,
                          uint32_t first_value,
                          const char *second_option_label,
                          uint32_t second_value,
                          uint32_t &selected_value) {
    bool changed = false;
    ImGui::Text("%s", label);
    ImGui::SameLine(first_option_x);
    if (ImGui::RadioButton(first_option_label, selected_value == first_value)) {
        selected_value = first_value;
        changed = true;
    }
    ImGui::SameLine(second_option_x);
    if (ImGui::RadioButton(second_option_label, selected_value == second_value)) {
        selected_value = second_value;
        changed = true;
    }
    return changed;
}

bool ShowCompactSelectionButton(const char *label, bool is_selected, const ButtonStyle &selected_style, const ImVec2 &size) {
    ScopedStyle style = MakeButtonStyle(is_selected ? selected_style : kCompactOptionButtonStyle);
    return ImGui::Button(label, size);
}

const ButtonStyle &GetSelectedRobotButtonStyle(const TeamRobotSelectionState &selection_state) {
    return selection_state.selected_team == 0 ? kSelectedRedRobotButtonStyle : kSelectedBlueRobotButtonStyle;
}

bool SubmitLoginSelection(RoboGo::io::MqttClient &mqtt_client, bool local_trial) {
    const auto &selection_state = GetTeamRobotSelectionState();
    const int current_robot_id = kLoginRobotIds[selection_state.selected_team][selection_state.selected_robot];
    if (local_trial) {
        SPDLOG_INFO("进入本地测试 client_id={}", current_robot_id);
        mqtt_client.set_client_id(std::to_string(current_robot_id));
        return true;
    }
    MqttServerAddressState &mqtt_server_state = GetMqttServerAddressState(mqtt_client);
    std::string mqtt_host;
    std::string mqtt_port;
    std::string parse_error;
    if (!ParseMqttServerEndpoint(mqtt_server_state.input, mqtt_host, mqtt_port, parse_error)) {
        mqtt_server_state.error_message = parse_error;
        SPDLOG_WARN("登录按钮点击失败，MQTT服务端地址非法 input={} reason={}",
                    mqtt_server_state.input,
                    parse_error);
        return false;
    }
    mqtt_server_state.error_message.clear();

    SPDLOG_INFO("登录按钮点击 selected_team={} selected_robot={} client_id={} mqtt_server={}:{}",
                selection_state.selected_team,
                selection_state.selected_robot,
                current_robot_id,
                mqtt_host,
                mqtt_port);
    mqtt_client.set_server_endpoint(std::move(mqtt_host), std::move(mqtt_port));
    mqtt_client.set_client_id(std::to_string(current_robot_id));
    return true;
}

float GetLoginPanelHeight(RoboGo::io::MqttClient::ConnectionState connection_state, bool local_trial) {
    const ImGuiStyle &style = ImGui::GetStyle();
    const float scale = ImGui::GetScale();
    const float text_line_height = ImGui::GetTextLineHeight();
    const float frame_height = ImGui::GetFrameHeight();
    const float team_button_height = std::max(frame_height, kLoginTeamButtonHeight * scale);
    const float robot_button_height = std::max(frame_height, kLoginRobotButtonHeight * scale);

    if (connection_state == RoboGo::io::MqttClient::ConnectionState::Connected) {
        const float logout_button_height = std::max(frame_height, text_line_height + kRecoverButtonStyle.padding.y * 2.0f);
        const float content_height = text_line_height +
                                     style.SeparatorSize +
                                     style.ItemSpacing.y * 2.0f +
                                     logout_button_height;
        return content_height + style.WindowPadding.y * 2.0f;
    }

    const float content_height = text_line_height +
                                 kLoginStatusGap * scale +
                                 text_line_height +
                                 team_button_height +
                                 kLoginSectionGap * scale +
                                 text_line_height +
                                 robot_button_height +
                                 kLoginSectionGap * scale +
                                 text_line_height +
                                 frame_height +
                                 text_line_height +
                                 text_line_height +
                                 kLoginTargetGap * scale +
                                 text_line_height +
                                 text_line_height;

    const float endpoint_height = kLoginSectionGap * scale + frame_height + text_line_height * 2.f;
    return content_height + style.WindowPadding.y * 2.0f + kLoginPanelBottomPadding * scale
           - (local_trial ? endpoint_height : 0.f);
}

void ShowLoginStatus(RoboGo::io::MqttClient::ConnectionState connection_state) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("连接状态");
    ImGui::SameLine(0.0f, 8.0f);
    ScopedStyle status_style = MakeTextColorStyle(GetLoginConnectionStateColor(connection_state));
    ImGui::TextUnformatted(GetLoginConnectionStateText(connection_state));
}

void ShowLoginTeamSelection(TeamRobotSelectionState &selection_state) {
    const float button_gap = ImGui::GetStyle().ItemSpacing.x;
    const float button_width = (ImGui::GetContentRegionAvail().x - button_gap) * 0.5f;
    const float button_height = kLoginTeamButtonHeight * ImGui::GetScale();

    ImGui::TextDisabled("队伍");
    if (ShowCompactSelectionButton("红队##team0",
                                   selection_state.selected_team == 0,
                                   kSelectedRedTeamButtonStyle,
                                   ImVec2(button_width, button_height))) {
        selection_state.selected_team = 0;
        SPDLOG_INFO("登录面板选择队伍=红队");
    }
    ImGui::SameLine();
    if (ShowCompactSelectionButton("蓝队##team1",
                                   selection_state.selected_team == 1,
                                   kSelectedBlueTeamButtonStyle,
                                   ImVec2(button_width, button_height))) {
        selection_state.selected_team = 1;
        SPDLOG_INFO("登录面板选择队伍=蓝队");
    }
}

void ShowLoginRobotSelection(TeamRobotSelectionState &selection_state, Competition competition, bool local_trial) {
    const float button_gap = ImGui::GetStyle().ItemSpacing.x;
    constexpr std::array alliance_roles{0, 2};
    constexpr std::array cup_roles{0, 1, 2, 3, 4};
    constexpr std::array cup_2027_roles{0, 2, 3};
    std::span<const int> roles = cup_roles;
    if (competition == Competition::Ul2027) roles = alliance_roles;
    if (competition == Competition::Uc2027) roles = cup_2027_roles;
    const bool offer_commander = competition == Competition::Uc2027 && local_trial;
    const float count = static_cast<float>(roles.size() + offer_commander);
    const float button_width = (ImGui::GetContentRegionAvail().x - button_gap * (count - 1)) / count;
    const float button_height = kLoginRobotButtonHeight * ImGui::GetScale();

    ImGui::TextDisabled("岗位");
    for (const int i : roles) {
        if (i != roles.front()) {
            ImGui::SameLine();
        }
        if (ShowCompactSelectionButton(RoleLabel(i, competition),
                                       !selection_state.commander && selection_state.selected_robot == i,
                                       GetSelectedRobotButtonStyle(selection_state),
                                       ImVec2(button_width, button_height))) {
            selection_state.selected_robot = i;
            selection_state.commander = false;
            SPDLOG_INFO("登录面板选择机器人 label={} robot_id={}",
                        RoleLabel(i, competition),
                        kLoginRobotIds[selection_state.selected_team][selection_state.selected_robot]);
        }
    }
    if (offer_commander) {
        ImGui::SameLine();
        if (ShowCompactSelectionButton("指挥手", selection_state.commander,
                                      GetSelectedRobotButtonStyle(selection_state), {button_width, button_height}))
            selection_state.commander = true;
    }
}

void ShowMqttServerEndpointInput(RoboGo::io::MqttClient &mqtt_client) {
    MqttServerAddressState &mqtt_server_state = GetMqttServerAddressState(mqtt_client);
    ImGui::TextDisabled("MQTT 服务端");
    ImGui::Dummy(ImVec2(0.0f, kLoginMqttInputTopMargin * ImGui::GetScale()));
    {
        ScopedStyle input_style = MakeInputStyle();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputText("##mqtt_server_address", &mqtt_server_state.input)) {
            mqtt_server_state.error_message.clear();
        }
    }
    if (!mqtt_server_state.error_message.empty()) {
        ScopedStyle error_style = MakeTextColorStyle(ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
        ImGui::TextWrapped("%s", mqtt_server_state.error_message.c_str());
    } else {
        ImGui::TextDisabled(" ");
    }
}

bool ShowLogoutPanel(RoboGo::io::MqttClient &mqtt_client, bool local_trial, bool over_ul_desk) {
    ScopedTitledPanel logout_panel("LogoutPanel", ImGui::GetContentRegionAvail().x, "登录控制", ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
    ScopedStyle logout_button_style = MakeButtonStyle(kRecoverButtonStyle);
    if (ImGui::Button(local_trial ? "结束测试 / 换队" : "退出登录")) {
        SPDLOG_INFO("退出登录按钮点击 client_id={}", mqtt_client.get_client_id());
        if (local_trial) return true;
        mqtt_client.set_client_id("");
        return over_ul_desk;
    }
    return false;
}

bool ShowTeamRobotSelection(RoboGo::io::MqttClient &mqtt_client, bool local_trial, Competition competition, bool over_ul_desk) {
    TeamRobotSelectionState &selection_state = GetTeamRobotSelectionState();
    const auto connection_state = mqtt_client.get_connection_state();
    const float selection_panel_height = GetLoginPanelHeight(connection_state, local_trial);
    ScopedChildWindow selection_panel("TeamRobotSelection",
                                      ImVec2(0, selection_panel_height),
                                      false,
                                      ImGuiWindowFlags_NoScrollbar);

    if (connection_state == RoboGo::io::MqttClient::ConnectionState::Connected) {
        return ShowLogoutPanel(mqtt_client, local_trial, over_ul_desk);
    }

    ScopedStyle compact_spacing_style;
    compact_spacing_style.PushVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 4.0f));

    if (!local_trial) {
        ShowLoginStatus(connection_state);
        ImGui::Dummy(ImVec2(0.0f, kLoginSectionGap * ImGui::GetScale()));
        ShowMqttServerEndpointInput(mqtt_client);
    }
    ImGui::Dummy(ImVec2(0.0f, kLoginStatusGap * ImGui::GetScale()));
    ShowLoginTeamSelection(selection_state);
    ImGui::Dummy(ImVec2(0.0f, kLoginSectionGap * ImGui::GetScale()));
    ShowLoginRobotSelection(selection_state, competition, local_trial);
    ImGui::Dummy(ImVec2(0.0f, kLoginTargetGap * ImGui::GetScale()));
    ImGui::TextDisabled("登录目标");
    ImGui::TextUnformatted(GetSelectedLoginTargetText(selection_state, competition).c_str());
    return false;
}

StartPanelVisibility GetStartPanelVisibility(const State &state) {
    uint32_t current_robot_id = GetCurrentRobotId(state);
    StartPanelVisibility visibility;
    visibility.show_ammo_panel = IsOneOf(current_robot_id, 1, 3, 4, 101, 103, 104);
    visibility.show_recover_panel = IsOneOf(current_robot_id, 1, 2, 3, 4, 101, 103, 104);
    visibility.show_revive_panel = IsOneOf(current_robot_id, 1, 2, 3, 4, 101, 102, 103, 104);
    visibility.show_deploy_panel = IsOneOf(current_robot_id, 1, 101);
    visibility.show_assembly_panel = IsOneOf(current_robot_id, 2, 102);
    visibility.show_marker_panel = IsOneOf(current_robot_id, 6, 106);
    visibility.show_robot_performance_options = IsOneOf(current_robot_id, 1, 3, 4, 101, 103, 104);
    visibility.show_sentry_performance_option = IsOneOf(current_robot_id, 6, 106);
    visibility.show_performance_panel = visibility.show_robot_performance_options || visibility.show_sentry_performance_option;
    visibility.show_dart_panel = visibility.show_marker_panel;
    visibility.show_air_support_panel = visibility.show_marker_panel;
    visibility.is_hero = IsOneOf(current_robot_id, 1, 101);
    return visibility;
}

void ShowDartPanel(const State &state, RoboGo::io::MqttClient &mqtt_client, float panel_width) {
    ScopedTitledPanel dart_panel("DartPanel", panel_width, "飞镖控制", ImVec4(1.0f, 0.4f, 0.8f, 1.0f));
    ImGui::AlignTextToFramePadding();

    DartPanelState &dart_panel_state = GetDartPanelState(state);

    float offset_from_start_x = ImGui::CalcTextSize("目标:").x + 10 * ImGui::GetScale();
    ImGui::Text("目标:");
    ImGui::SameLine(offset_from_start_x);
    float target_row_start_x = ImGui::GetCursorPosX();
    float target_row_start_y = ImGui::GetCursorPosY();
    float compact_row_gap = 2.0f * ImGui::GetScale();

    for (uint32_t i = 0; i < kDartTargetNames.size(); i++) {
        if (i == 3) {
            ImGui::SetCursorPosY(target_row_start_y + ImGui::GetFrameHeight() + compact_row_gap);
            ImGui::SetCursorPosX(target_row_start_x);
        } else if (i != 0) {
            ImGui::SameLine(0, 10);
        }
        if (ImGui::RadioButton(kDartTargetNames[i], dart_panel_state.target_id == (i + 1))) {
            dart_panel_state.target_id = i + 1;
            SendDartCommand(mqtt_client, dart_panel_state.target_id, dart_panel_state.open, false);
        }
    }

    ImGui::Spacing();

    ScopedStyle dart_button_style = MakeButtonStyle(kDartButtonStyle);

    if (ImGui::Button(dart_panel_state.open ? "关闭闸门" : "开启闸门")) {
        dart_panel_state.open = !dart_panel_state.open;
        SendDartCommand(mqtt_client, dart_panel_state.target_id, dart_panel_state.open, false);
    }

    ImGui::SameLine();

    ButtonFeedback launch_feedback = GetDartLaunchFeedback(state);
    if (ButtonWithTooltip("确认发射", launch_feedback.valid_tooltip, launch_feedback.disabled_reason)) {
        SendDartCommand(mqtt_client, dart_panel_state.target_id, dart_panel_state.open, true, true);
    }
}

void ShowAirSupportPanel(const State &state, RoboGo::io::MqttClient &mqtt_client, float panel_width) {
    ScopedTitledPanel air_support_panel("AirSupportPanel", panel_width, "空中支援", ImVec4(0.2f, 0.85f, 1.0f, 1.0f));

    if (state.air_support_status_sync.has_value()) {
        const auto &sync = *state.air_support_status_sync;
        ImGui::Text("当前状态: %s", GetAirSupportStatusName(sync.airsupport_status()));
        ImGui::Text("免费剩余: %u秒", sync.left_time());
        ImGui::SameLine();
        ImGui::Text("已花费: %u", sync.cost_coins());
        ImGui::Text("照射状态: %s", sync.is_being_targeted() != 0 ? "被照射" : "未被照射");
        ImGui::SameLine();
        ImGui::Text("发射机构: %s", GetAirSupportShooterStatusName(sync.shooter_status()));
    } else {
        ImGui::TextDisabled("当前状态: 等待同步");
        ImGui::TextDisabled("免费剩余: 未知");
        ImGui::TextDisabled("照射状态: 未知");
    }

    ImGui::Spacing();

    ScopedStyle air_support_button_style = MakeButtonStyle(kAirSupportButtonStyle);

    ButtonFeedback free_feedback = GetFreeAirSupportFeedback(state);
    ButtonFeedback paid_feedback = GetPaidAirSupportFeedback(state);
    ButtonFeedback cancel_feedback = GetCancelAirSupportFeedback(state);
    ButtonFeedback release_feedback = GetReleaseAirSupportCountermeasureFeedback(state);

    if (ButtonWithTooltip("免费呼叫", free_feedback.valid_tooltip, free_feedback.disabled_reason)) {
        SendAirSupportCommand(mqtt_client, 1);
    }
    ImGui::SameLine();
    if (ButtonWithTooltip("付费呼叫", paid_feedback.valid_tooltip, paid_feedback.disabled_reason)) {
        SendAirSupportCommand(mqtt_client, 2);
    }
    ImGui::SameLine();
    if (ButtonWithTooltip("取消呼叫", cancel_feedback.valid_tooltip, cancel_feedback.disabled_reason)) {
        SendAirSupportCommand(mqtt_client, 0);
    }
    if (ButtonWithTooltip("解除反制", release_feedback.valid_tooltip, release_feedback.disabled_reason)) {
        SendAirSupportCommand(mqtt_client, 3);
    }
}

void ShowDartAndAirSupportRow(const State &state,
                              RoboGo::io::MqttClient &mqtt_client,
                              bool show_dart_panel,
                              bool show_air_support_panel) {
    ShowPanelRow("DartAirSupportRow", 6.0f, {
            {.visible = show_dart_panel, .render = [&](float width) { ShowDartPanel(state, mqtt_client, width); }},
            {.visible = show_air_support_panel, .render = [&](float width) { ShowAirSupportPanel(state, mqtt_client, width); }},
    });
}

void ShowAmmoPanel(const State &state, RoboGo::io::MqttClient &mqtt_client, float panel_width, bool is_hero) {
    ScopedTitledPanel ammo_panel("AmmoPanel", panel_width, "弹药兑换", ImVec4(0.3f, 1.0f, 0.3f, 1.0f));

    AmmoPanelState &ammo_panel_state = GetAmmoPanelState();
    bool is_valid_ammo;
    long long ammo;
    {
        ScopedStyle input_style = MakeInputStyle();
        IntegerInputText("请输入允许发弹量：", &ammo_panel_state.ammo_str, &is_valid_ammo, &ammo);
    }

    ImGui::Spacing();

    ButtonFeedback ammo_feedback = GetAmmoExchangeFeedback(state, is_valid_ammo, ammo, is_hero);
    ButtonFeedback remote_ammo_feedback = GetRemoteAmmoExchangeFeedback(state, is_valid_ammo, ammo, is_hero);

    {
        ScopedStyle ammo_button_style = MakeButtonStyle(kPrimaryButtonStyle);
        if (ButtonWithTooltip("兑换允许发弹量", ammo_feedback.valid_tooltip, ammo_feedback.disabled_reason)) {
            SendCommonCommand(mqtt_client, is_hero ? 1 : 2, static_cast<uint32_t>(ammo));
        }
        ImGui::SameLine();
        if (ButtonWithTooltip("远程兑换", remote_ammo_feedback.valid_tooltip, remote_ammo_feedback.disabled_reason)) {
            SendCommonCommand(mqtt_client, 5);
        }
    }
}

void ShowAssemblyPanel(const State &state, RoboGo::io::MqttClient &mqtt_client, float panel_width) {
    ScopedTitledPanel assembly_panel("AssemblyPanel", panel_width, "装配控制", ImVec4(0.3f, 0.8f, 1.0f, 1.0f));

    AssemblyPanelState &assembly_panel_state = GetAssemblyPanelState();
    bool is_valid_assembly_difficulty = false;
    long long assembly_difficulty;
    {
        ScopedStyle input_style = MakeInputStyle();
        IntegerInputText("请输入装配难度：", &assembly_panel_state.difficulty_str, &is_valid_assembly_difficulty, &assembly_difficulty);
    }

    ImGui::Spacing();

    {
        ScopedStyle assembly_button_style = MakeButtonStyle(kAssemblyButtonStyle);
        ImGui::BeginDisabled(!state.tech_core_motion_state_sync.has_value() || state.tech_core_motion_state_sync.value().basic_state() == 1);
        if (ImGui::Button("取消装配")) {
            SendAssemblyCommand(mqtt_client, 0, 0);
        }
        ImGui::EndDisabled();

        ImGui::SameLine();

        ImGui::BeginDisabled(!is_valid_assembly_difficulty || !state.tech_core_motion_state_sync.has_value() || state.tech_core_motion_state_sync.value().basic_state() > 1);
        if (ImGui::Button("确认装配")) {
            SendAssemblyCommand(mqtt_client, static_cast<uint32_t>(assembly_difficulty), 1);
        }
        ImGui::EndDisabled();
    }
}

void ShowDeployPanel(const State &state, RoboGo::io::MqttClient &mqtt_client, float panel_width) {
    ScopedTitledPanel deploy_panel("DeployPanel", panel_width, "部署模式", ImVec4(0.7f, 0.3f, 1.0f, 1.0f));

    bool has_deploy_mode_status = state.deploy_mode_status_sync.has_value();
    const char *label = !has_deploy_mode_status ? "进入/退出部署模式" : state.deploy_mode_status_sync->status() == 0 ? "进入部署模式" : "退出部署模式";
    const char *disabled_reason = has_deploy_mode_status ? nullptr : "无法获取当前是否进入了部署模式";
    {
        ScopedStyle deploy_button_style = MakeButtonStyle(kDeployButtonStyle);
        if (ButtonWithTooltip(label, "", disabled_reason)) {
            SendDeployModeCommand(mqtt_client, state.deploy_mode_status_sync->status() == 0 ? 1 : 0);
        }
    }
}

void ShowRecoverPanel(const State &state, RoboGo::io::MqttClient &mqtt_client, float panel_width) {
    ScopedTitledPanel recover_panel("RecoverPanel", panel_width, "远程回血", ImVec4(1.0f, 0.3f, 0.3f, 1.0f));

    ButtonFeedback recover_feedback = GetRecoverFeedback(state);
    {
        ScopedStyle recover_button_style = MakeButtonStyle(kRecoverButtonStyle);
        if (ButtonWithTooltip("远程回血", recover_feedback.valid_tooltip, recover_feedback.disabled_reason)) {
            SendCommonCommand(mqtt_client, 6);
        }
    }
}

void ShowRevivePanel(const State &state, RoboGo::io::MqttClient &mqtt_client, float panel_width) {
    ScopedTitledPanel revive_panel("RevivePanel", panel_width, "复活控制", ImVec4(1.0f, 1.0f, 0.3f, 1.0f));

    ButtonFeedback confirm_feedback = GetConfirmReviveFeedback(state);
    ButtonFeedback instant_feedback = GetInstantReviveFeedback(state);
    {
        ScopedStyle revive_button_style = MakeButtonStyle(kReviveButtonStyle);
        if (ButtonWithTooltip("确认复活", confirm_feedback.valid_tooltip, confirm_feedback.disabled_reason)) {
            SendCommonCommand(mqtt_client, 3);
        }

        ImGui::SameLine();

        if (ButtonWithTooltip("兑换立即复活", instant_feedback.valid_tooltip, instant_feedback.disabled_reason)) {
            SendCommonCommand(mqtt_client, 4);
        }
    }
}

void ShowMarkerPanel(float panel_width) {
    ScopedTitledPanel marker_panel("MarkerPanel", panel_width, "地图标记", ImVec4(1.0f, 0.6f, 0.2f, 1.0f));

    {
        ScopedStyle marker_button_style = MakeButtonStyle(kMarkerButtonStyle);
        if (is_marker_mode()) {
            if (ImGui::Button("退出地图点位标记模式")) {
                exit_marker_mode();
            }
        } else {
            if (ImGui::Button("开始标记地图点位")) {
                enter_marker_mode();
            }
        }
    }

    bool include_sentry = is_marker_mode_include_sentry();
    ImGui::Spacing();
    if (ImGui::RadioButton("包含哨兵", include_sentry)) {
        set_marker_mode_include_sentry(true);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("不包含哨兵", !include_sentry)) {
        set_marker_mode_include_sentry(false);
    }

    ImGui::Spacing();
}

void ShowPerformancePanel(const State &state,
                          RoboGo::io::MqttClient &mqtt_client,
                          float panel_width,
                          bool is_hero,
                          bool show_robot_performance_options,
                          bool show_sentry_performance_option) {
    ScopedTitledPanel performance_panel("PerformancePanel", panel_width, "性能选择", ImVec4(0.3f, 0.8f, 1.0f, 1.0f));
    ImGui::AlignTextToFramePadding();

    bool is_send = false;
    PerformancePanelState &performance_panel_state = GetPerformancePanelState(state);

    float offset_from_start_x1 = ImGui::CalcTextSize("发射机构:").x + 10 * ImGui::GetScale();
    float offset_from_start_x2 = offset_from_start_x1 + 150 * ImGui::GetScale();
    const bool alliance = state.competition == RoboGo::app::Competition::Ul2027;
    enum Field : size_t { Shooter, Chassis, Sentry };
    const auto choice = [&](Field field, const char *label, uint32_t &selected,
                            const char *first, uint32_t a, const char *second, uint32_t b) {
        if (!ShowLabeledRadioPair(label, offset_from_start_x1, offset_from_start_x2, first, a, second, b, selected)) return;
        is_send = true;
        if (alliance) {
            const std::string_view name = selected == a ? first : second;
            performance_panel_state.requested[field] = PerformancePanelState::Choice{
                std::string(name.substr(0, name.find("##"))), selected, std::chrono::steady_clock::now()};
        }
    };
    if (show_robot_performance_options) {
        if (is_hero) {
            choice(Shooter, "发射机构:", performance_panel_state.shooter, "近战优先##shooter3", 3, "远程优先##shooter4", 4);
        } else {
            choice(Shooter, "发射机构:", performance_panel_state.shooter, "冷却优先##shooter1", 1, "爆发优先##shooter2", 2);
        }

        if (is_hero) {
            choice(Chassis, "底盘:", performance_panel_state.chassis, "近战优先##chassis3", 3, "远程优先##chassis4", 4);
        } else {
            choice(Chassis, "底盘:", performance_panel_state.chassis, "血量优先##chassis1", 1, "功率优先##chassis2", 2);
        }
    }

    if (show_sentry_performance_option) {
        choice(Sentry, "哨兵:", performance_panel_state.sentry_control, "自动##sentry0", 0, "半自动##sentry1", 1);
    }

    if (is_send) {
        SendPerformanceSelection(mqtt_client, performance_panel_state);
    }
    if (alliance) {
        const auto feedback = [&](Field field, const char *group, const std::optional<ReturnedPreference> &reply) {
            const auto &request = performance_panel_state.requested[field];
            if (!request) return;
            const bool newer = reply && reply->received_at > request->selected_at;
            const char *status = !newer ? "等待回执" : reply->value == request->value ? "回传一致" : "回传值不同";
            ImGui::TextWrapped("%s申请：%s · %s", group, request->label.c_str(), status);
        };
        feedback(Shooter, "发射机构", state.shooter_return);
        feedback(Chassis, "底盘", state.chassis_return);
        feedback(Sentry, "哨兵", state.sentry_return);
        if (std::ranges::none_of(performance_panel_state.requested, [](const auto &r) { return r.has_value(); })) {
            ImGui::TextDisabled("点选后可查看裁判端回执");
        }
    }
}

void ShowAmmoAssemblyDeployRow(const State &state,
                               RoboGo::io::MqttClient &mqtt_client,
                               const StartPanelVisibility &visibility) {
    ShowPanelRow("AmmoAssemblyDeployRow", 4.0f, {
            {.visible = visibility.show_ammo_panel, .render = [&](float width) { ShowAmmoPanel(state, mqtt_client, width, visibility.is_hero); }},
            {.visible = visibility.show_assembly_panel, .render = [&](float width) { ShowAssemblyPanel(state, mqtt_client, width); }},
            {.visible = visibility.show_deploy_panel, .render = [&](float width) { ShowDeployPanel(state, mqtt_client, width); }},
    });
}

void ShowRecoverReviveRow(const State &state,
                          RoboGo::io::MqttClient &mqtt_client,
                          const StartPanelVisibility &visibility) {
    ShowPanelRow("RecoverReviveRow", 3.0f, {
            {.visible = visibility.show_recover_panel, .render = [&](float width) { ShowRecoverPanel(state, mqtt_client, width); }},
            {.visible = visibility.show_revive_panel, .render = [&](float width) { ShowRevivePanel(state, mqtt_client, width); }},
    });
}

void ShowMarkerPerformanceRow(const State &state,
                              RoboGo::io::MqttClient &mqtt_client,
                              const StartPanelVisibility &visibility) {
    const bool alliance = state.competition == RoboGo::app::Competition::Ul2027;
    ShowPanelRow("MarkerPerformanceRow", alliance ? 6.f : 4.f, {
            {.visible = visibility.show_marker_panel, .render = [&](float width) { ShowMarkerPanel(width); }},
            {.visible = visibility.show_performance_panel, .render = [&](float width) {
                 ShowPerformancePanel(state,
                                      mqtt_client,
                                      width,
                                      visibility.is_hero,
                                      visibility.show_robot_performance_options,
                                      visibility.show_sentry_performance_option);
             }},
    });
}

void ShowMouseSensitivityRow() {
    ImGui::Dummy(ImVec2(0.0f, kMouseSensitivityTopGap * ImGui::GetScale()));

    ScopedChildWindow mouse_sensitivity_row("MouseSensitivityRow",
                                            ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 2),
                                            false,
                                            ImGuiWindowFlags_NoScrollbar);

    {
        ScopedStyle label_style = MakeTextColorStyle(ImVec4(1.0f, 0.8f, 0.3f, 1.0f));
        ImGui::SetCursorPosX(0);
        ImGui::Text("鼠标灵敏度");
    }

    ScopedStyle slider_style = MakeMouseSensitivitySliderStyle();

    float mouse_sensitivity = get_mouse_sensitivity();
    ImGui::SameLine(ImGui::CalcTextSize("鼠标灵敏度").x + 10 * ImGui::GetScale());
    ImGui::SetNextItemWidth(-10 * ImGui::GetScale());
    if (ImGui::SliderFloat("##mouse_sensitivity", &mouse_sensitivity, 0.05f, 3.0f, "%.2f")) {
        set_mouse_sensitivity(mouse_sensitivity);
    }
}

MenuAction ShowMenuFooter(RoboGo::io::MqttClient &client, bool local_trial, bool over_ul_desk, Competition competition) {
    const bool logged_in = client.get_connection_state() == RoboGo::io::MqttClient::ConnectionState::Connected;
    const float width = std::max(1.f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * .5f);
    const ImVec2 size(width, kPrimaryActionButtonHeight * ImGui::GetScale());
    if (ImGui::Button("返回赛事选择", size)) return MenuAction::ReturnToEvents;
    ImGui::SameLine();
    ScopedStyle primary_button_style = MakeEnterGameButtonStyle();
    const char *label = logged_in ? (over_ul_desk ? "回到比赛画面" : "进入比赛")
                                  : (local_trial ? "开始测试" : "登录");
    if (ImGui::Button(label, size)) {
        if (!logged_in) {
            const auto &seat = GetTeamRobotSelectionState();
            if (local_trial && competition == Competition::Uc2027 && seat.commander)
                return seat.selected_team == 0 ? MenuAction::CommanderRed : MenuAction::CommanderBlue;
            (void)SubmitLoginSelection(client, local_trial);
        } else if (over_ul_desk) {
            return MenuAction::Resume;
        } else {
            enter_game_mode();
        }
    }
    return MenuAction::None;
}

} // namespace

void ResetStartPanelSession(RoboGo::app::Competition competition) {
    menu_inputs = {};
    if (competition == RoboGo::app::Competition::Ul2027) menu_inputs.selection.selected_robot = 2;
}

ImRect MatchMenuBounds() {
    const auto *vp = ImGui::GetMainViewport();
    const float inset_x = std::min(24.f, vp->WorkSize.x * .03f);
    const float inset_y = std::min(16.f, vp->WorkSize.y * .03f);
    const ImVec2 size(std::min(750 * ImGui::GetScale(), vp->WorkSize.x - 2 * inset_x),
                      std::min(500 * ImGui::GetScale(), vp->WorkSize.y - 2 * inset_y));
    const ImVec2 pos(vp->WorkPos.x + (vp->WorkSize.x - size.x) * .5f,
                     vp->WorkPos.y + (vp->WorkSize.y - size.y) * .5f);
    return ImRect(pos, ImVec2(pos.x + size.x, pos.y + size.y));
}

void PositionMatchMenu() {
    const ImRect bounds = MatchMenuBounds();
    ImGui::SetNextWindowPos(bounds.Min);
    ImGui::SetNextWindowSize(bounds.GetSize());
}

MenuAction StartPanel(const State &state, RoboGo::io::MqttClient &client, bool local_trial,
                      bool over_ul_desk, bool waiting_for_video) {
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    ScopedStyle window_style = MakeStartPanelWindowStyle();
    if (!ImGui::Begin("StartPanel", nullptr, flags)) {
        ImGui::End();
        return MenuAction::None;
    }
    // Raise the menu above HUD windows without stealing focus from its input fields.
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    MenuAction action = MenuAction::None;
    if (over_ul_desk) {
        ImGui::TextUnformatted("比赛菜单");
        ImGui::SameLine();
        ImGui::TextDisabled("兑换、回血和复活均为申请，结果由裁判端回传");
    } else {
        ImGui::Text("%s  /  %s", RoboGo::app::CompetitionName(state.competition),
                    local_trial ? "本地测试" : "正式登录");
        if (waiting_for_video && client.get_connection_state() != RoboGo::io::MqttClient::ConnectionState::Connected) {
            ImGui::SameLine();
            ImGui::TextDisabled("等待图传");
        }
    }
    luojiamaster::brand::MenuSignature();
    ImGui::Separator();
    const float footer = kPrimaryActionButtonHeight * ImGui::GetScale() + ImGui::GetStyle().ItemSpacing.y;
    if (ImGui::BeginChild("比赛菜单内容", ImVec2(0, -footer))) {
        if (ShowTeamRobotSelection(client, local_trial, state.competition, over_ul_desk)) action = MenuAction::ReturnToEvents;
        if (client.get_connection_state() == RoboGo::io::MqttClient::ConnectionState::Connected) {
            const StartPanelVisibility visibility = GetStartPanelVisibility(state);
            ShowAmmoAssemblyDeployRow(state, client, visibility);
            ShowRecoverReviveRow(state, client, visibility);
            ShowMarkerPerformanceRow(state, client, visibility);
            ShowDartAndAirSupportRow(state, client, visibility.show_dart_panel, visibility.show_air_support_panel);
        }
        ShowMouseSensitivityRow();
    }
    ImGui::EndChild();
    const MenuAction footer_action = ShowMenuFooter(client, local_trial, over_ul_desk, state.competition);
    ImGui::End();
    return action == MenuAction::None ? footer_action : action;
}
