#include "header_panel.h"
#include "panel/brand_mark.h"
#include "app/match_values.h"
#include "panel/hud_text.h"

const char *GetGameResultName(uint32_t game_result);

namespace {
using RoboGo::app::kUlStartingControlDegree;

float TextWidth(const char *text, float font) {
    return ImGui::GetFont()->CalcTextSizeA(font, FLT_MAX, 0.f, text).x;
}

void CenteredText(ImDrawList *draw, float x, float top, float font, const char *text, ImU32 color = IM_COL32_WHITE) {
    hud::Outline(draw, {x - TextWidth(text, font) * .5f, top}, color, text, font);
}

void DrawTeamBars(const State &state, bool blue, float left, float right, float top, float font) {
    auto *draw = ImGui::GetWindowDrawList();
    const ImU32 team = blue ? IM_COL32(3, 187, 245, 255) : IM_COL32(250, 63, 64, 255);
    const ImU32 track = blue ? IM_COL32(12, 67, 102, 165) : IM_COL32(104, 35, 45, 165);
    const float bar_height = font * 1.61f;
    const ImVec2 start{left, top}, end{right, top + bar_height};
    const float rounding = bar_height * .25f;
    const auto corners = blue ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersLeft;
    draw->AddRectFilled(start, end, track, rounding, corners);
    const auto control = RoboGo::app::ControlDegree2027(state, blue);
    if (control && *control > 0) {
        // Both bars start at the clock, as in the 2026 scoreboard.
        const float fill = (right - left) * std::min(*control, kUlStartingControlDegree) / kUlStartingControlDegree;
        draw->AddRectFilled({blue ? left : right - fill, top}, {blue ? left + fill : right, end.y},
                            team, rounding, corners);
    }
    draw->AddRect(start, end, team, rounding, 1.f, corners);
    const char *label = blue ? "蓝方控制度" : "红方控制度";
    char points[32] = "等待";
    if (control) ImFormatString(points, IM_ARRAYSIZE(points), "%u / %u", *control, kUlStartingControlDegree);
    const float padding = font * .55f;
    const float text_y = top + (bar_height - font) * .5f;
    hud::Outline(draw, {blue ? right - padding - TextWidth(label, font) : left + padding, text_y},
                 IM_COL32_WHITE, label, font);
    hud::Outline(draw, {blue ? left + padding : right - padding - TextWidth(points, font), text_y},
                 IM_COL32_WHITE, points, font);

    constexpr std::array<int, 3> roles{1, 3, 7};
    constexpr std::array<const char *, 3> names{"重装 1", "步兵 3", "哨兵 7"};
    const float gap = font * .55f;
    const float width = (right - left - 2.f * gap) / 3.f;
    const float names_top = end.y + font * .20f;
    const float rail_top = names_top + font * 1.05f;
    const float rail_bottom = rail_top + font * .35f;
    for (size_t i = 0; i < roles.size(); ++i) {
        const float x = left + static_cast<float>(i) * (width + gap);
        const float center = x + width * .5f;
        const auto hp = RoboGo::app::Health2027(state.robot_static_status, state.global_unit_status, blue, roles[i]);
        const auto cap = RoboGo::app::HealthCap2027(state, blue, roles[i]);
        CenteredText(draw, center, names_top, font, names[i]);
        draw->AddRectFilled({x, rail_top}, {x + width, rail_bottom}, IM_COL32(32, 37, 44, 177));
        char number[32] = "等待";
        if (hp) {
            // Unconfirmed max: absolute scale with a tick every 100 HP, not a percentage.
            const uint32_t scale = cap.value_or(roles[i] == 1 ? 450u : 350u);
            const float filled = width * (std::min(*hp, scale) / static_cast<float>(scale));
            if (filled > 0.f) draw->AddRectFilled({x, rail_top}, {x + filled, rail_bottom}, team);
            if (cap) {
                ImFormatString(number, IM_ARRAYSIZE(number), "%u/%u", *hp, *cap);
            } else {
                ImFormatString(number, IM_ARRAYSIZE(number), "%u 点", *hp);
                for (uint32_t tick = 100; tick < scale; tick += 100) {
                    const float tx = x + width * tick / scale;
                    draw->AddLine({tx, rail_top}, {tx, rail_bottom}, IM_COL32(235, 241, 245, 225));
                }
                if (ImGui::IsMouseHoveringRect({x, names_top}, {x + width, rail_bottom + 1.3f * font}))
                    ImGui::SetTooltip("每格 100 点血，满血值待确认");
            }
        }
        draw->AddRect({x, rail_top}, {x + width, rail_bottom}, IM_COL32(235, 241, 245, 174));
        CenteredText(draw, center, rail_bottom + font * .13f, font, number);
    }
}
} // namespace

bool ShowAllianceHeader(const State &state, bool local_trial, bool blue, bool heavy) {
    bool menu = false;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("HeaderWindow", nullptr, flags)) {
        const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        const float native_font = ImGui::GetFontSize();
        const float font = std::min({std::clamp(size.x / 80.f, native_font, native_font * 1.6f),
                                     size.y / 5.2f, size.x / 49.f});
        const float center = pos.x + size.x * .5f;
        const float center_half_width = 5.5f * font;
        const float top = pos.y + font * .32f;
        DrawTeamBars(state, false, pos.x + font, center - center_half_width, top, font);
        DrawTeamBars(state, true, center + center_half_width, pos.x + size.x - font, top, font);

        auto *draw = ImGui::GetWindowDrawList();
        const auto &game = state.game_status;
        char seat[64] = {};
        ImFormatString(seat, IM_ARRAYSIZE(seat), "%s · %s%s", local_trial ? "本地测试" : "高校联盟赛",
                       blue ? "蓝方" : "红方", heavy ? "重装" : "步兵");
        CenteredText(draw, center, top - .15f * font, .91f * font, seat);
        char clock[32] = "--:--";
        if (game && game->has_stage_countdown_sec() && game->stage_countdown_sec() >= 0) {
            const int seconds = game->stage_countdown_sec();
            ImFormatString(clock, IM_ARRAYSIZE(clock), "%02d:%02d", seconds / 60, seconds % 60);
        }
        const bool paused = game && game->has_is_paused() && game->is_paused();
        CenteredText(draw, center, top + .76f * font, 1.76f * font, clock,
                     paused ? IM_COL32(255, 212, 118, 255) : IM_COL32_WHITE);
        luojiamaster::brand::DrawHudMarkInGap(draw, center - center_half_width + .2f * font,
                center - TextWidth(clock, 1.76f * font) * .5f - .32f * font,
                top + 1.65f * font, 1.05f * font);
        constexpr std::array<const char *, 6> phases{"未开赛", "准备", "自检", "开赛倒计时", "比赛中", "结算"};
        const char *phase = "等待赛况";
        if (game && game->has_current_stage() && game->current_stage() < phases.size()) {
            phase = paused ? "已暂停" : phases[game->current_stage()];
            if (game->current_stage() == 5 && game->has_game_result()) {
                if (const char *result = GetGameResultName(game->game_result())) phase = result;
            }
        }
        char status[72] = {};
        if (game && game->has_current_round())
            ImFormatString(status, IM_ARRAYSIZE(status), "第 %u 局 · %s", game->current_round(), phase);
        else
            ImFormatString(status, IM_ARRAYSIZE(status), "局数待定 · %s", phase);
        const float stage_width = TextWidth(status, font);
        const float stage_font = font * std::min(1.f, (2.f * center_half_width - .5f * font) / stage_width);
        CenteredText(draw, center, top + 2.55f * font, stage_font, status);

        const char *menu_label = "比赛菜单  Esc";
        const float button_width = ImGui::CalcTextSize(menu_label).x + 2.f * ImGui::GetStyle().FramePadding.x;
        ImGui::SetCursorScreenPos({center - button_width * .5f, top + 3.68f * font});
        menu = ImGui::SmallButton(menu_label);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    return menu;
}

void AddTextInTheCenterOfRect(ImDrawList *draw_list, const char *text_begin, const char *text_end, float left, float top, float right, float bottom, ImU32 col) {
    ImVec2 text_size = ImGui::CalcTextSize(text_begin, text_end);
    draw_list->AddText(ImVec2(left + (right - left) / 2 - text_size.x / 2, top + (bottom - top) / 2 - text_size.y / 2), col, text_begin, text_end);
}

const char *GetGameResultName(uint32_t game_result) {
    switch (game_result) {
        case 0:
            return "平局";
        case 1:
            return "红方胜利";
        case 2:
            return "蓝方胜利";
        default:
            return nullptr;
    }
}

std::string CupPhaseText(const GameStatus *game) {
    if (!game) return "等待赛况";
    if (game->has_current_stage() && game->current_stage() == 5) {
        const char *winner = game->has_game_result() ? GetGameResultName(game->game_result()) : nullptr;
        std::string phase = winner ? winner : "比赛结算";
        constexpr std::array<const char *, 8> reasons{
            "", "基地被摧毁", "累计控制度", "全队总伤害", "全队剩余血量", "平局", "异常终止", "裁判判定"};
        if (game->has_end_reason() && game->end_reason() > 0 && game->end_reason() < reasons.size()) {
            const char *reason = reasons[game->end_reason()];
            if (phase != reason) phase.append(" · ").append(reason);
        }
        return phase;
    }
    if (game->has_is_paused() && game->is_paused()) return "已暂停";
    constexpr std::array<const char *, 5> phases{"未开赛", "准备阶段", "裁判自检", "开赛倒计时", "比赛中"};
    if (!game->has_current_stage()) return "等待赛况";
    return game->current_stage() < phases.size() ? phases[game->current_stage()] : "未知阶段";
}

std::string CupTimeText(const GameStatus *game) {
    if (!game || !game->has_stage_countdown_sec() || game->stage_countdown_sec() < 0) return "--:--";
    const auto seconds = game->stage_countdown_sec();
    char buffer[32];
    ImFormatString(buffer, IM_ARRAYSIZE(buffer), "%02d:%02d", seconds / 60, seconds % 60);
    return buffer;
}

namespace {
void CupCenteredText(ImDrawList *draw, float center, float top, float font, float width,
                     const char *text, ImU32 color = IM_COL32_WHITE) {
    const float fitted_font = font * std::min(1.f, width / std::max(1.f, TextWidth(text, font)));
    CenteredText(draw, center, top, fitted_font, text, color);
}

// The neutral outpost meter belongs to a fixed R/B location, independent of who controls it.
void DrawCupHealthMeter(ImDrawList *draw, float left, float right, float top, float height,
                        const char *label, std::optional<uint32_t> hp, uint32_t cap,
                        ImU32 tint, bool fill_from_right, float font) {
    const ImVec2 start{left, top}, end{right, top + height};
    const float radius = height * .18f;
    draw->AddRectFilled(start, end, IM_COL32(27, 35, 43, 145), radius);
    if (hp && *hp > 0) {
        const float width = (right - left) * static_cast<float>(std::min(*hp, cap)) / static_cast<float>(cap);
        draw->AddRectFilled({fill_from_right ? right - width : left, top},
                            {fill_from_right ? right : left + width, end.y}, tint, radius);
    }
    draw->AddRect(start, end, tint, radius);
    char number[32] = "等待";
    if (hp) ImFormatString(number, IM_ARRAYSIZE(number), "%u / %u", *hp, cap);
    const float inset = font * .45f;
    const float combined_width = TextWidth(label, font) + TextWidth(number, font) + inset;
    const float text_font = font * std::min(1.f, std::max(1.f, right - left - 2.f * inset) / combined_width);
    const float y = top + .5f * (height - text_font);
    hud::Outline(draw, {left + inset, y}, IM_COL32_WHITE, label, text_font);
    hud::Outline(draw, {right - inset - TextWidth(number, text_font), y}, IM_COL32_WHITE, number, text_font);
}

void DrawCupTeam(ImDrawList *draw, const State &state, bool blue, float left, float right,
                 float top, float font) {
    const ImU32 tint = blue ? IM_COL32(21, 157, 201, 245) : IM_COL32(201, 64, 68, 245);
    DrawCupHealthMeter(draw, left, right, top, 1.44f * font, blue ? "蓝方基地" : "红方基地",
                       RoboGo::app::CupBaseHp(state, blue), 10000, tint, !blue, font);

    const float width = right - left;
    const float gap = .5f * font;
    const float outpost_width = .55f * width;
    const float station_left = blue ? right - outpost_width : left;
    const float cp_center = blue ? (left + station_left - gap) * .5f : (left + outpost_width + gap + right) * .5f;
    DrawCupHealthMeter(draw, station_left, station_left + outpost_width, top + 1.65f * font,
                       1.20f * font, blue ? "B 前哨站" : "R 前哨站", RoboGo::app::CupOutpostHp(state, blue),
                       1000, IM_COL32(102, 125, 140, 215), !blue, .91f * font);
    char control[64] = "累计控制度  等待";
    if (const auto cp = RoboGo::app::ControlDegree2027(state, blue))
        ImFormatString(control, IM_ARRAYSIZE(control), "累计控制度  %u", *cp);
    CupCenteredText(draw, cp_center, top + 1.74f * font, font, width - outpost_width - gap, control);

    constexpr std::array<int, 4> ids{1, 3, 4, 7};
    constexpr std::array<const char *, 4> names{"重装 1", "步兵 3", "步兵 4", "哨兵 7"};
    const float slot = width / ids.size();
    for (size_t i = 0; i < ids.size(); ++i) {
        const float x = left + slot * (static_cast<float>(i) + .5f);
        const auto hp = RoboGo::app::Health2027(state.robot_static_status, state.global_unit_status, blue, ids[i]);
        const float y = top + 3.18f * font;
        // Other seats' maximum HP is not sent in the public roster. Show the actual reading.
        CupCenteredText(draw, x, y, .88f * font, slot - gap, names[i]);
        char hp_text[32] = "等待";
        if (hp) ImFormatString(hp_text, IM_ARRAYSIZE(hp_text), "%u", *hp);
        CupCenteredText(draw, x, y + 1.04f * font, font, slot - gap, hp_text,
                        hp && *hp == 0 ? IM_COL32(255, 199, 113, 255) : IM_COL32_WHITE);
    }
}

void DrawCupHeader(const State &state, bool local_trial) {
    auto *draw = ImGui::GetWindowDrawList();
    const auto pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    const float font = std::max(1.f, std::min({size.x / 85.f, size.y / 6.15f, 34.f * ImGui::GetScale()}));
    const float center = pos.x + size.x * .5f, half = 6.4f * font;
    const float top = pos.y + .30f * font;
    DrawCupTeam(draw, state, false, pos.x + font, center - half, top, font);
    DrawCupTeam(draw, state, true, center + half, pos.x + size.x - font, top, font);

    const auto *game = state.game_status ? &*state.game_status : nullptr;
    const auto time = CupTimeText(game);
    const auto phase = CupPhaseText(game);
    const bool paused = game && game->has_is_paused() && game->is_paused();
    const float center_width = 2.f * half - font * .4f;
    CupCenteredText(draw, center, top, .92f * font, center_width, "2027 超级对抗赛");
    CupCenteredText(draw, center, top + .92f * font, 1.65f * font, center_width, time.c_str(),
                    paused ? IM_COL32(255, 211, 127, 255) : IM_COL32_WHITE);
    luojiamaster::brand::DrawHudMarkInGap(draw, center - half + .3f * font,
            center - .5f * TextWidth(time.c_str(), 1.65f * font) - .3f * font, top + 1.72f * font, font);
    CupCenteredText(draw, center, top + 2.65f * font, font, center_width, phase.c_str());

    std::string round = "局数待定";
    if (game && game->has_current_round()) {
        round = "第 " + std::to_string(game->current_round());
        if (game->has_total_rounds()) round += "/" + std::to_string(game->total_rounds());
        round += " 局";
    }
    const auto red = game && game->has_red_score() ? std::to_string(game->red_score()) : "等待";
    const auto blue = game && game->has_blue_score() ? std::to_string(game->blue_score()) : "等待";
    const auto score = round + " · 比分 " + red + " : " + blue;
    CupCenteredText(draw, center, top + 3.80f * font, .88f * font, center_width, score.c_str());

    const auto own_blue = RoboGo::app::BlueIdentity2027(state.robot_static_status);
    const auto role = own_blue ? state.robot_static_status->robot_id() % 100u : 0u;
    const char *seat = role == 1 ? "重装 1" : role == 3 ? "步兵 3" : role == 4 ? "步兵 4" : nullptr;
    std::string identity = local_trial ? "本地测试" : "正式";
    if (seat) identity.append(*own_blue ? " · 蓝方" : " · 红方").append(seat);
    else identity += " · 身份待确认";
    CupCenteredText(draw, center, top + 4.89f * font, .86f * font, center_width, identity.c_str());
}
} // namespace

void ShowHeaderPanel(const State &state, bool local_trial) {
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("HeaderWindow", nullptr, window_flags)) {
        ImGui::End();
        return;
    }
    if (state.competition == RoboGo::app::Competition::Uc2027) {
        DrawCupHeader(state, local_trial);
        ImGui::End();
        return;
    }
    const ImVec2 window_pos = ImGui::GetWindowPos();
    const ImVec2 window_size = ImGui::GetWindowSize();
    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    const char *text_begin, *text_end;

    uint32_t current_stage;
    int32_t stage_countdown_sec;
    bool is_paused;
    if (state.game_status.has_value()) {
        current_stage = state.game_status->current_stage();
        stage_countdown_sec = state.game_status->stage_countdown_sec();
        is_paused = state.game_status->is_paused();
        if (current_stage == 0) {
            stage_countdown_sec = 0;
        }
    } else {
        current_stage = 0;
        stage_countdown_sec = 0;
        is_paused = false;
    }

    // 比赛阶段显示
    ImGui::PushFont(ImGui::GetFont(), 14);
    const char *stage_text;
    std::string stage_text_storage;
    if (state.game_status.has_value()) {
        if (is_paused) {
            stage_text = "技术暂停中";
        } else {
            switch (current_stage) {
                case 0:
                    stage_text = "未开始比赛";
                    break;
                case 1:
                    stage_text = "准备阶段";
                    break;
                case 2:
                    stage_text = "裁判系统自检";
                    break;
                case 3:
                    stage_text = "五秒倒计时";
                    break;
                case 4:
                    stage_text = "比赛中";
                    break;
                case 5:
                    if (state.game_status->has_game_result()) {
                        const char *game_result_name = GetGameResultName(state.game_status->game_result());
                        if (game_result_name != nullptr) {
                            stage_text_storage = std::string("比赛结算: ") + game_result_name;
                            stage_text = stage_text_storage.c_str();
                            break;
                        }
                    }
                    stage_text = "比赛结算中";
                    break;
                default:
                    stage_text = "未知阶段";
                    break;
            }
        }
    } else {
        stage_text = "未连接";
    }
    ImVec2 stage_text_size = ImGui::CalcTextSize(stage_text);
    float stage_left = window_pos.x + (window_size.x - stage_text_size.x) / 2;
    float stage_top = window_pos.y + 12 * ImGui::GetScale() + 5 * ImGui::GetScale();
    draw_list->AddText(ImVec2(stage_left, stage_top), IM_COL32(255, 255, 255, 255), stage_text);
    ImGui::PopFont();

    // 倒计时
    ImGui::PushFont(ImGui::GetFont(), 22);
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%02d:%02d", stage_countdown_sec / 60, stage_countdown_sec % 60);
    ImVec2 countdown_text_size = ImGui::CalcTextSize(text_begin, text_end);
    float countdown_left = window_pos.x + (window_size.x - countdown_text_size.x) / 2;
    float countdown_right = countdown_left + countdown_text_size.x;
    float countdown_top = stage_top + stage_text_size.y + 10 * ImGui::GetScale();
    float countdown_bottom = countdown_top + 20 * ImGui::GetScale();
    draw_list->AddText(ImVec2(countdown_left, countdown_top + (countdown_bottom - countdown_top - countdown_text_size.y) / 2), IM_COL32(255, 255, 255, 255), text_begin, text_end);
    ImGui::PopFont();

    // 金币显示
    uint32_t gold = state.global_logistics_status.has_value() ? state.global_logistics_status->remaining_economy() : 0;
    ImGui::PushFont(ImGui::GetFont(), 18);
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%d", gold);
    const ImVec2 economyTextSize = ImGui::CalcTextSize(text_begin, text_end);
    const ImVec2 economyPos{window_pos.x + (window_size.x - economyTextSize.x) * .5f,
                            countdown_bottom + 10 * ImGui::GetScale()};
    draw_list->AddText(economyPos, IM_COL32(255, 215, 0, 255), text_begin, text_end);
    luojiamaster::brand::DrawHudMarkInGap(draw_list, countdown_left - 20.f * ImGui::GetScale(),
            economyPos.x - .42f * economyTextSize.y, economyPos.y + .5f * economyTextSize.y,
            economyTextSize.y);
    ImGui::PopFont();

    // 前哨站血条和基地血条
    ImU32 red_color = IM_COL32(250, 63, 64, 255);
    ImU32 blue_color = IM_COL32(3, 187, 245, 255);
    ImU32 red_color_translated = IM_COL32(250, 63, 64, 50);
    ImU32 blue_color_translated = IM_COL32(3, 187, 245, 50);
    float rounding = 15 * ImGui::GetScale();
    float base_health_horizontal_padding = 24 * ImGui::GetScale();
    float outpost_length_ratio = 0.2f;
    float building_health_top = window_pos.y + 12 * ImGui::GetScale();
    float building_health_bottom = building_health_top + 30 * ImGui::GetScale();
    float red_health_left = window_pos.x + base_health_horizontal_padding;
    float red_health_right = countdown_left - base_health_horizontal_padding;
    float blue_health_left = countdown_right + base_health_horizontal_padding;
    float blue_health_right = window_pos.x + window_size.x - base_health_horizontal_padding;
    float red_outpost_health_left = red_health_left;
    float red_outpost_health_right = red_health_left + (red_health_right - red_health_left) * outpost_length_ratio - base_health_horizontal_padding / 2;
    float red_base_health_left = red_health_left + (red_health_right - red_health_left) * outpost_length_ratio + base_health_horizontal_padding / 2;
    float red_base_health_right = red_health_right;
    float blue_outpost_health_left = blue_health_right - (blue_health_right - blue_health_left) * outpost_length_ratio + base_health_horizontal_padding / 2;
    float blue_outpost_health_right = blue_health_right;
    float blue_base_health_left = blue_health_left;
    float blue_base_health_right = blue_health_right - (blue_health_right - blue_health_left) * outpost_length_ratio - base_health_horizontal_padding / 2;
    uint32_t outpost_max_health = 1500;
    uint32_t base_max_health = 5000;
    uint32_t red_outpost_health, red_base_health, red_base_shield, blue_outpost_health, blue_base_health, blue_base_shield;
    if (state.global_unit_status.has_value() && state.robot_static_status.has_value()) {
        if (state.robot_static_status->robot_id() < 100) {
            red_outpost_health = state.global_unit_status->outpost_health();
            red_base_health = state.global_unit_status->base_health();
            red_base_shield = state.global_unit_status->base_shield();
            blue_outpost_health = state.global_unit_status->enemy_outpost_health();
            blue_base_health = state.global_unit_status->enemy_base_health();
            blue_base_shield = state.global_unit_status->enemy_base_shield();
        } else {
            red_outpost_health = state.global_unit_status->enemy_outpost_health();
            red_base_health = state.global_unit_status->enemy_base_health();
            red_base_shield = state.global_unit_status->enemy_base_shield();
            blue_outpost_health = state.global_unit_status->outpost_health();
            blue_base_health = state.global_unit_status->base_health();
            blue_base_shield = state.global_unit_status->base_shield();
        }
    } else {
        red_outpost_health = 0;
        red_base_health = 0;
        red_base_shield = 0;
        blue_outpost_health = 0;
        blue_base_health = 0;
        blue_base_shield = 0;
    }

    // 绘制血条背景
    draw_list->AddRectFilled(ImVec2(red_outpost_health_left, building_health_top), ImVec2(red_outpost_health_right, building_health_bottom), red_color_translated, rounding, ImDrawFlags_RoundCornersLeft);
    draw_list->AddRectFilled(ImVec2(red_base_health_left, building_health_top), ImVec2(red_base_health_right, building_health_bottom), red_color_translated, rounding, ImDrawFlags_RoundCornersLeft);
    draw_list->AddRectFilled(ImVec2(blue_outpost_health_left, building_health_top), ImVec2(blue_outpost_health_right, building_health_bottom), blue_color_translated, rounding, ImDrawFlags_RoundCornersRight);
    draw_list->AddRectFilled(ImVec2(blue_base_health_left, building_health_top), ImVec2(blue_base_health_right, building_health_bottom), blue_color_translated, rounding, ImDrawFlags_RoundCornersRight);

    // 绘制血条填充
    if (red_outpost_health > 0) {
        draw_list->AddRectFilled(ImVec2(red_outpost_health_left + (red_outpost_health_right - red_outpost_health_left) * (1 - static_cast<float>(red_outpost_health) / static_cast<float>(outpost_max_health)), building_health_top), ImVec2(red_outpost_health_right, building_health_bottom), red_color, rounding, ImDrawFlags_RoundCornersLeft);
    }
    if (red_base_health > 0) {
        draw_list->AddRectFilled(ImVec2(red_base_health_left + (red_base_health_right - red_base_health_left) * (1 - static_cast<float>(red_base_health) / static_cast<float>(base_max_health)), building_health_top), ImVec2(red_base_health_right, building_health_bottom), red_color, rounding, ImDrawFlags_RoundCornersLeft);
    }
    if (blue_outpost_health > 0) {
        draw_list->AddRectFilled(ImVec2(blue_outpost_health_left, building_health_top), ImVec2(blue_outpost_health_left + (blue_outpost_health_right - blue_outpost_health_left) * (static_cast<float>(blue_outpost_health) / static_cast<float>(outpost_max_health)), building_health_bottom), blue_color, rounding, ImDrawFlags_RoundCornersRight);
    }
    if (blue_base_health > 0) {
        draw_list->AddRectFilled(ImVec2(blue_base_health_left, building_health_top), ImVec2(blue_base_health_left + (blue_base_health_right - blue_base_health_left) * (static_cast<float>(blue_base_health) / static_cast<float>(base_max_health)), building_health_bottom), blue_color, rounding, ImDrawFlags_RoundCornersRight);
    }

    // 绘制血条边框
    draw_list->AddRect(ImVec2(red_outpost_health_left, building_health_top), ImVec2(red_outpost_health_right, building_health_bottom), red_color, rounding, 1.0f, ImDrawFlags_RoundCornersLeft);
    draw_list->AddRect(ImVec2(red_base_health_left, building_health_top), ImVec2(red_base_health_right, building_health_bottom), red_color, rounding, 1.0f, ImDrawFlags_RoundCornersLeft);
    draw_list->AddRect(ImVec2(blue_outpost_health_left, building_health_top), ImVec2(blue_outpost_health_right, building_health_bottom), blue_color, rounding, 1.0f, ImDrawFlags_RoundCornersRight);
    draw_list->AddRect(ImVec2(blue_base_health_left, building_health_top), ImVec2(blue_base_health_right, building_health_bottom), blue_color, rounding, 1.0f, ImDrawFlags_RoundCornersRight);

    // 血条文本
    ImGui::PushFont(ImGui::GetFont(), 16);
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%d/%d", red_outpost_health, outpost_max_health);
    AddTextInTheCenterOfRect(draw_list, text_begin, text_end, red_outpost_health_left, building_health_top, red_outpost_health_right, building_health_bottom, IM_COL32(255, 255, 255, 255));
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%d/%d", red_base_health + red_base_shield, base_max_health);
    AddTextInTheCenterOfRect(draw_list, text_begin, text_end, red_base_health_left, building_health_top, red_base_health_right, building_health_bottom, IM_COL32(255, 255, 255, 255));
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%d/%d", blue_outpost_health, outpost_max_health);
    AddTextInTheCenterOfRect(draw_list, text_begin, text_end, blue_outpost_health_left, building_health_top, blue_outpost_health_right, building_health_bottom, IM_COL32(255, 255, 255, 255));
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%d/%d", blue_base_health + blue_base_shield, base_max_health);
    AddTextInTheCenterOfRect(draw_list, text_begin, text_end, blue_base_health_left, building_health_top, blue_base_health_right, building_health_bottom, IM_COL32(255, 255, 255, 255));
    ImGui::PopFont();

    // 机器人血量条
    float robot_bar_top = building_health_bottom + 30 * ImGui::GetScale();
    float robot_bar_bottom = robot_bar_top + 4 * ImGui::GetScale();
    float red_robot_bar_left = red_health_left;
    float red_robot_bar_right = red_health_right;
    float blue_robot_bar_left = blue_health_left;
    float blue_robot_bar_right = blue_health_right;
    const char *robot_names[] = {"英雄", "工程", "步兵", "步兵", "步兵", "无人机", "哨兵", "飞镖", "雷达"};
    static_assert(sizeof(state.red_robots) / sizeof(state.red_robots[0]) == sizeof(robot_names) / sizeof(robot_names[0]));
    int ids[] = {1, 2, 3, 4, 6, 7, 8, 9};
    int ids_size = std::size(ids);
    const bool legacy = state.competition == RoboGo::app::Competition::Uc2026;
    const auto format_health = [&](bool blue, int id, const RobotStatus &robot, bool show_bar) {
        if (legacy && show_bar) {
            ImFormatStringToTempBuffer(&text_begin, &text_end, "%u/%u", robot.health, robot.max_health);
        } else if (auto hp = RoboGo::app::Health2027(state.robot_static_status, state.global_unit_status, blue, id); !legacy && hp) {
            ImFormatStringToTempBuffer(&text_begin, &text_end, "%u", *hp);
        } else {
            ImFormatStringToTempBuffer(&text_begin, &text_end, "-");
        }
    };
    float robot_bar_spacing = 6.0f * ImGui::GetScale();
    ImU32 bg_color = IM_COL32(50, 50, 50, 255);
    float red_robot_bar_item_width = (red_robot_bar_right - red_robot_bar_left - robot_bar_spacing * static_cast<float>(ids_size - 1)) / static_cast<float>(ids_size);
    float blue_robot_bar_item_width = (blue_robot_bar_right - blue_robot_bar_left - robot_bar_spacing * static_cast<float>(ids_size - 1)) / static_cast<float>(ids_size);
    ImGui::PushFont(ImGui::GetFont(), 12);
    for (int i = 0; i < ids_size; i++) {
        int id = ids[i];
        const RobotStatus &robot = state.red_robots[id - 1];
        float bar_left = red_robot_bar_left + static_cast<float>(i) * (red_robot_bar_item_width + robot_bar_spacing);
        float bar_right = bar_left + red_robot_bar_item_width;
        bool has_health = id != 6 && id != 8 && id != 9;// 无人机，飞镖，雷达站没有血条
        bool show_health = has_health && robot.max_health > 0;
        draw_list->AddRectFilled(ImVec2(bar_left, robot_bar_top), ImVec2(bar_right, robot_bar_bottom), bg_color, 2.0f);
        if (show_health) {
            float ratio = static_cast<float>(robot.health) / static_cast<float>(robot.max_health);
            if (ratio > 1.0f) {
                ratio = 1.0f;
            }
            if (ratio > 0.0f) {
                draw_list->AddRectFilled(ImVec2(bar_left, robot_bar_top), ImVec2(bar_left + (bar_right - bar_left) * ratio, robot_bar_bottom), red_color, 2.0f);
            }
        }
        ImFormatStringToTempBuffer(&text_begin, &text_end, "%d %s", id, robot_names[id - 1]);
        ImVec2 id_size = ImGui::CalcTextSize(text_begin, text_end);
        draw_list->AddText(ImVec2(bar_left + (red_robot_bar_item_width - id_size.x) / 2, robot_bar_top - 2 - id_size.y), IM_COL32(255, 255, 255, 255), text_begin, text_end);
        format_health(false, id, robot, show_health);
        ImVec2 health_size = ImGui::CalcTextSize(text_begin, text_end);
        float health_text_y = robot_bar_bottom + 2;
        draw_list->AddText(ImVec2(bar_left + (red_robot_bar_item_width - health_size.x) / 2, health_text_y), IM_COL32(255, 255, 255, 255), text_begin, text_end);
        if (!legacy) continue;
        // 显示允许发弹量
        ImFormatStringToTempBuffer(&text_begin, &text_end, "弹药:%d", robot.bullets);
        ImVec2 bullets_size = ImGui::CalcTextSize(text_begin, text_end);
        draw_list->AddText(ImVec2(bar_left + (red_robot_bar_item_width - bullets_size.x) / 2, health_text_y + health_size.y + 1), IM_COL32(200, 200, 200, 255), text_begin, text_end);
    }
    for (int i = 0; i < ids_size; i++) {
        int id = ids[i];
        const RobotStatus &robot = state.blue_robots[id - 1];
        float bar_left = blue_robot_bar_left + static_cast<float>(i) * (blue_robot_bar_item_width + robot_bar_spacing);
        float bar_right = bar_left + blue_robot_bar_item_width;
        bool has_health = id != 6 && id != 8 && id != 9;// 无人机，飞镖，雷达站没有血条
        bool show_health = has_health && robot.max_health > 0;
        draw_list->AddRectFilled(ImVec2(bar_left, robot_bar_top), ImVec2(bar_right, robot_bar_bottom), bg_color, 2.0f);
        if (show_health) {
            float ratio = static_cast<float>(robot.health) / static_cast<float>(robot.max_health);
            if (ratio > 1.0f) {
                ratio = 1.0f;
            }
            if (ratio > 0.0f) {
                draw_list->AddRectFilled(ImVec2(bar_left, robot_bar_top), ImVec2(bar_left + (bar_right - bar_left) * ratio, robot_bar_bottom), blue_color, 2.0f);
            }
        }
        ImFormatStringToTempBuffer(&text_begin, &text_end, "%d %s", id, robot_names[id - 1]);
        ImVec2 id_size = ImGui::CalcTextSize(text_begin, text_end);
        draw_list->AddText(ImVec2(bar_left + (blue_robot_bar_item_width - id_size.x) / 2, robot_bar_top - 2 - id_size.y), IM_COL32(255, 255, 255, 255), text_begin, text_end);
        format_health(true, id, robot, show_health);
        ImVec2 health_size = ImGui::CalcTextSize(text_begin, text_end);
        float health_text_y = robot_bar_bottom + 2;
        draw_list->AddText(ImVec2(bar_left + (blue_robot_bar_item_width - health_size.x) / 2, health_text_y), IM_COL32(255, 255, 255, 255), text_begin, text_end);
        if (!legacy) continue;
        // 显示允许发弹量
        ImFormatStringToTempBuffer(&text_begin, &text_end, "弹药:%d", robot.bullets);
        ImVec2 bullets_size = ImGui::CalcTextSize(text_begin, text_end);
        draw_list->AddText(ImVec2(bar_left + (blue_robot_bar_item_width - bullets_size.x) / 2, health_text_y + health_size.y + 1), IM_COL32(200, 200, 200, 255), text_begin, text_end);
    }
    ImGui::PopFont();

    ImGui::End();
}
