#include "robot_status_panel.h"
#include "hud_text.h"

namespace {
template<class Message>
std::optional<uint32_t> Received(const std::optional<Message> &message,
                                 bool (Message::*present)() const, uint32_t (Message::*value)() const) {
    if (message && ((*message).*present)()) return ((*message).*value)();
    return std::nullopt;
}

std::string NumberOrWait(std::optional<uint32_t> value) {
    return value ? std::to_string(*value) : "等待";
}

void AllianceMeter(const char *label, std::optional<uint32_t> current,
                   std::optional<uint32_t> limit, const ImVec4 &color) {
    hud::Text(ImGui::GetColorU32(color), "%s  %s / %s", label, NumberOrWait(current).c_str(), NumberOrWait(limit).c_str());
    if (current && limit && *limit != 0) {
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
        ImGui::ProgressBar(std::min(1.f, static_cast<float>(*current) / *limit), {-1.f, 3.f}, "");
        ImGui::PopStyleColor();
    }
}

void AllianceStatus(const State &state) {
    const auto &s = state.robot_static_status;
    const auto &d = state.robot_dynamic_status;
    hud::Label("本车状态");
    ImGui::Separator();
    AllianceMeter("血量", Received(d, &RobotDynamicStatus::has_current_health, &RobotDynamicStatus::current_health),
                  Received(s, &RobotStaticStatus::has_max_health, &RobotStaticStatus::max_health),
                  {1.f,.68f,.64f,1.f});
    AllianceMeter("缓冲能量", Received(d, &RobotDynamicStatus::has_current_buffer_energy, &RobotDynamicStatus::current_buffer_energy),
                  Received(s, &RobotStaticStatus::has_max_buffer_energy, &RobotStaticStatus::max_buffer_energy),
                  {.6f,.85f,1.f,1.f});
    const auto level = Received(s, &RobotStaticStatus::has_level, &RobotStaticStatus::level);
    const auto exp = Received(d, &RobotDynamicStatus::has_current_experience, &RobotDynamicStatus::current_experience);
    const auto to_upgrade = Received(d, &RobotDynamicStatus::has_experience_for_upgrade, &RobotDynamicStatus::experience_for_upgrade);
    hud::Text(hud::white, "等级 %s   经验 %s", NumberOrWait(level).c_str(), NumberOrWait(exp).c_str());
    hud::Text(hud::white, "升级还需 %s", NumberOrWait(to_upgrade).c_str());
    const auto ammo = Received(d, &RobotDynamicStatus::has_remaining_ammo, &RobotDynamicStatus::remaining_ammo);
    hud::Text(hud::white, "允许发弹量  %s", NumberOrWait(ammo).c_str());
    ImGui::Separator();
    hud::Label("设备状态");
    const auto &module = state.robot_module_status;
    using M = RobotModuleStatus;
    struct Module {
        const char *name;
        bool (M::*present)() const;
        uint32_t (M::*value)() const;
    };
    constexpr Module modules[]{
        {"主控", &M::has_main_controller, &M::main_controller}, {"电源", &M::has_power_manager, &M::power_manager},
        {"图传", &M::has_video_transmission, &M::video_transmission}, {"RFID", &M::has_rfid, &M::rfid},
        {"定位", &M::has_uwb, &M::uwb}, {"装甲板", &M::has_armor, &M::armor},
        {"17毫米", &M::has_small_shooter, &M::small_shooter}, {"42毫米", &M::has_big_shooter, &M::big_shooter},
        {"超级电容", &M::has_capacitor, &M::capacitor}, {"灯条", &M::has_light_strip, &M::light_strip},
    };
    int received = 0, online = 0;
    ImGuiTextBuffer offline;
    for (const auto &m : modules) {
        auto value = Received(module, m.present, m.value);
        if (!value) continue;
        ++received;
        if (*value == 1) ++online;
        else offline.appendf("%s%s", offline.empty() ? "" : "、", m.name);
    }
    if (received == 0) hud::Label("等待设备上报", hud::muted);
    else {
        hud::Text(hud::white, "已上报 %d 项 · 在线 %d 项", received, online);
        if (!offline.empty()) hud::Label((std::string("离线：") + offline.c_str()).c_str(), hud::white, true);
    }
}
}

void ShowProgressBar(const ImVec4 &bar_color, float ratio, float width, float height) {
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar_color);
    ImGui::ProgressBar(ratio, ImVec2(width, height), "");
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

void ShowRobotStatusPanel(const State &state) {
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("VehicleStatusPanel", nullptr, window_flags)) {
        ImGui::End();
        return;
    }
    if (state.competition == RoboGo::app::Competition::Ul2027) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {ImGui::GetStyle().ItemSpacing.x, 2.f});
        AllianceStatus(state);
        ImGui::PopStyleVar();
        ImGui::End();
        return;
    }

    // 获取可用内容区域大小
    ImVec2 content_avail = ImGui::GetContentRegionAvail();
    float progress_bar_height = 4.0f;

    // Panel 标题
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::Text("机器人状态");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // TODO show buffer energy bar
    uint32_t current_health, max_health, current_energy, max_energy, current_experience, experience_for_upgrade, level;
    float health_ratio, energy_ratio, experience_ratio;
    if (state.robot_static_status.has_value() && state.robot_dynamic_status.has_value()) {
        current_health = state.robot_dynamic_status->current_health();
        max_health = state.robot_static_status->max_health();
        health_ratio = static_cast<float>(current_health) / static_cast<float>(max_health);
        current_energy = state.robot_dynamic_status->current_chassis_energy();
        max_energy = state.robot_static_status->max_chassis_energy();
        energy_ratio = static_cast<float>(current_energy) / static_cast<float>(max_energy);
        current_experience = state.robot_dynamic_status->current_experience();
        experience_for_upgrade = state.robot_dynamic_status->experience_for_upgrade();
        level = state.robot_static_status->level();
        experience_ratio = static_cast<float>(current_experience) / static_cast<float>(experience_for_upgrade);
    } else {
        current_health = 0;
        max_health = 0;
        current_energy = 0;
        max_energy = 0;
        current_experience = 0;
        experience_for_upgrade = 0;
        level = 0;
        health_ratio = 0.0f;
        energy_ratio = 0.0f;
        experience_ratio = 0.0f;
    }

    // 血条
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
    ImGui::Text("血量: %d/%d", current_health, max_health);
    ImGui::PopStyleColor();
    ShowProgressBar(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), health_ratio, content_avail.x, progress_bar_height);

    // 能量条
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.7f, 1.0f, 1.0f));
    ImGui::Text("能量: %d/%d", current_energy, max_energy);
    ImGui::PopStyleColor();
    ShowProgressBar(ImVec4(0.3f, 0.7f, 1.0f, 1.0f), energy_ratio, content_avail.x, progress_bar_height);

    // 经验条
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.3f, 1.0f));
    ImGui::Text("经验: %d/%d (等级: %d)", current_experience, experience_for_upgrade, level);
    ImGui::PopStyleColor();
    ShowProgressBar(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), experience_ratio, content_avail.x, progress_bar_height);

    // 允许发弹量
    uint32_t remaining_ammo;
    if (state.robot_dynamic_status.has_value()) {
        remaining_ammo = state.robot_dynamic_status->remaining_ammo();
    } else {
        remaining_ammo = 0;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.8f, 1.0f, 1.0f));
    ImGui::Text("允许发弹量: %d", remaining_ammo);
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // 指示灯
    bool is_auto_aim = state.auto_aim;
    bool is_gyro = state.gyro;
    bool is_deploy_mode;
    if (state.deploy_mode_status_sync.has_value()) {
        is_deploy_mode = state.deploy_mode_status_sync->status() == 1;
    } else {
        is_deploy_mode = false;
    }

    float text_width1 = ImGui::CalcTextSize("指示灯: ").x;
    float text_width2 = ImGui::CalcTextSize("自瞄").x;
    float text_width3 = ImGui::CalcTextSize("小陀螺").x;
    float text_width4 = ImGui::CalcTextSize("部署模式").x;
    float indicator_spacing = (content_avail.x - text_width1 - text_width2 - text_width3 - text_width4) / 4.0f;
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "指示灯: ");
    ImGui::SameLine(text_width1 + indicator_spacing);
    ImGui::TextColored(is_auto_aim ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f) : ImVec4(0.5, 0.5, 0.5, 1), "自瞄");
    ImGui::SameLine(text_width1 + text_width2 + indicator_spacing * 2);
    ImGui::TextColored(is_gyro ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f) : ImVec4(0.5, 0.5, 0.5, 1), "小陀螺");
    ImGui::SameLine(text_width1 + text_width2 + text_width3 + indicator_spacing * 3);
    ImGui::TextColored(is_deploy_mode ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f) : ImVec4(0.5, 0.5, 0.5, 1), "部署模式");
    ImGui::Spacing();

    // 设备连接状态
    const char *module_names[] = {
            "主控", "电源管理", "图传", "RFID", "UWB",
            "装甲板", "小弹丸", "大弹丸", "超级电容", "灯条",
            "激光检测"};

    ImGui::SameLine(0, 0);
    ImGui::TextColored(ImVec4(0.8f, 0.4f, 1.0f, 1.0f), "未连接设备: ");

    if (state.robot_module_status.has_value()) {
        uint32_t module_status[] = {
                state.robot_module_status->main_controller(),
                state.robot_module_status->power_manager(),
                state.robot_module_status->video_transmission(),
                state.robot_module_status->rfid(),
                state.robot_module_status->uwb(),
                state.robot_module_status->armor(),
                state.robot_module_status->small_shooter(),
                state.robot_module_status->big_shooter(),
                state.robot_module_status->capacitor(),
                state.robot_module_status->light_strip(),
                state.robot_module_status->laser_detection_module()};
        static_assert(std::size(module_names) == std::size(module_status));

        ImGui::SameLine(0, 0);
        ImGuiTextBuffer buffer;
        for (size_t i = 0; i < std::size(module_names); i++) {
            if (module_status[i] != 1) {
                if (!buffer.empty()) {
                    buffer.append(", ");
                }
                buffer.appendf("%s", module_names[i]);
            }
        }
        if (buffer.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "（无）");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", buffer.c_str());
        }
    } else {
        ImGui::SameLine(0, 0);
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "未知");
    }

    ImGui::End();
}
