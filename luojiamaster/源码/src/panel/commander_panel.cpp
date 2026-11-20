#include "commander_panel.h"
#include "panel/brand_mark.h"

namespace {

using RoboGo::arena::Mode;
constexpr std::array<const char *, 3> kSeats{"重装 1 号", "步兵 3 号", "步兵 4 号"};
constexpr ImGuiWindowFlags kNoScroll = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

const char *MapName(Mode mode) {
    switch (mode) {
        case Mode::Light: return "精简三维";
        case Mode::Detailed: return "精细三维";
        case Mode::Flat: return "二维俯视";
        default: return "等待场馆";
    }
}

void DrawVideo(CommanderDeskState &desk, const TeammateViewState &feeds, int slot, ImVec2 size,
               CommanderDeskActions &actions, int &clicked) {
    ImGui::PushID(slot);
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImRect rect(a, ImVec2(a.x + size.x, a.y + size.y));
    auto *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(rect.Min, rect.Max, IM_COL32(20, 28, 33, 255), 4.f);
    const float pad = std::min(8.f, std::min(size.x, size.y) * .06f);
    ImRect content = rect;
    content.Expand(-pad);
    if (desk.focused < 0 && slot == 0) {
        const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
        desk.thumbnail_pixels = {content.GetWidth() * scale.x,
            (content.GetHeight() - ImGui::GetTextLineHeightWithSpacing()) * scale.y};
    }
    TeammatePeerView waiting{};
    waiting.label = kSeats[slot];
    const auto &feed = slot < feeds.count ? feeds.peers[slot] : waiting;
    if (DrawTeammateTile(draw, feed, content, desk.zoom[slot], true))
        actions.drawn_mask |= 1u << slot;
    if (ImGui::InvisibleButton("画面", size)) clicked = slot;
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        desk.zoom[slot] = std::clamp(desk.zoom[slot] + ImGui::GetIO().MouseWheel * .25f, 1.f, 4.f);
    }
    ImGui::PopID();
}

void SignalMenu(CommanderDeskState &desk, const TeammateViewState &feeds,
                RoboGo::app::CommanderMarkerReplay::Time frame_time, CommanderDeskActions &actions) {
    if (ImGui::Button("测试信号")) ImGui::OpenPopup("##signals");
    if (!ImGui::BeginPopup("##signals")) return;
    for (int slot = 0; slot < static_cast<int>(kSeats.size()); ++slot) {
        const bool paused = slot < feeds.count && feeds.peers[slot].paused;
        const std::string label = std::string(paused ? "恢复 " : "断开 ") + kSeats[slot];
        if (ImGui::MenuItem(label.c_str(), nullptr, false, slot < feeds.count)) actions.toggle_feed = slot;
    }
    ImGui::Separator();
    if (ImGui::MenuItem(desk.replay.Blue3Paused() ? "恢复蓝 3 位置" : "暂停蓝 3 位置"))
        desk.replay.ToggleBlue3(frame_time);
    ImGui::MenuItem("显示车辆棋子", nullptr, &desk.show_markers);
    ImGui::EndPopup();
}

} // namespace

CommanderDeskActions ShowCommanderDesk(CommanderDeskState &desk, const TeammateViewState &feeds,
                                       ImTextureID field_plan, RoboGo::app::CommanderMarkerReplay::Time frame_time,
                                       double benchmark_seconds) {
    CommanderDeskActions actions;
    const auto *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    const float base = ImGui::GetFontSize();
    const float wanted = std::clamp(2.f * std::floor(std::min(viewport->Size.x / 95.f, viewport->Size.y / 52.f) / 2.f),
                                    base, std::max(32.f, base));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * wanted / base);
    const float pad = ImGui::GetFontSize() * .65f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {pad, pad});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {pad * .7f, pad * .65f});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(.045f,.071f,.082f,1.f));
    const bool visible = ImGui::Begin("指挥席###commander", nullptr, ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | kNoScroll);
    if (visible) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("指挥席");
        ImGui::SameLine();
        ImGui::TextDisabled("本地测试 · %s", desk.team == 0 ? "红方" : "蓝方");
        ImGui::SameLine();
        ImGui::BeginDisabled(benchmark_seconds >= 0.);
        SignalMenu(desk, feeds, frame_time, actions);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("返回赛事")) actions.return_to_events = true;
        const auto headerEnd = ImGui::GetItemRectMax();
        luojiamaster::brand::DrawHudMarkInGap(ImGui::GetWindowDrawList(), headerEnd.x + pad,
                viewport->WorkPos.x + viewport->WorkSize.x - pad,
                (ImGui::GetItemRectMin().y + headerEnd.y) * .5f, ImGui::GetFontSize() * 1.25f);
        ImGui::Separator();
        int clicked = -1;
        if (desk.focused >= 0 && desk.focused < static_cast<int>(kSeats.size())) {
            ImGui::TextDisabled("单击画面回到总览 · 滚轮缩放");
            const ImVec2 area = ImGui::GetContentRegionAvail();
            DrawVideo(desk, feeds, desk.focused, {std::max(1.f,area.x), std::max(1.f,area.y)}, actions, clicked);
        } else {
            const ImVec2 area = ImGui::GetContentRegionAvail();
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float map_width = std::max(1.f, (area.x - gap) * .70f);
            if (ImGui::BeginChild("场馆", {map_width,0.f}, ImGuiChildFlags_None, kNoScroll)) {
                ImGui::SetNextItemWidth(std::min(map_width * .36f, ImGui::GetFontSize() * 8.f));
                if (ImGui::BeginCombo("##map-choice", MapName(desk.requested_mode))) {
                    for (const Mode mode : {Mode::Light, Mode::Detailed, Mode::Flat})
                        if (ImGui::Selectable(MapName(mode), desk.requested_mode == mode))
                            desk.requested_mode = mode;
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Button("回到俯视")) RoboGo::arena::ResetCamera();
                if (map_width > ImGui::GetFontSize() * 26.f) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(desk.requested_mode == Mode::Flat ? "左键平移 · 滚轮指哪放哪" : "左键平移 · 右键旋转 · 滚轮指哪放哪");
                }
                const ImVec2 available = ImGui::GetContentRegionAvail();
                const float status_height = ImGui::GetTextLineHeightWithSpacing();
                const ImVec2 size(std::max(1.f,available.x), std::max(1.f,available.y - status_height));
                static const std::vector<RoboGo::arena::ReplayMarker> no_markers;
                const auto& markers = desk.show_markers ? desk.replay.Visible(frame_time) : no_markers;
                actions.actual_mode = RoboGo::arena::Draw(size, desk.requested_mode, field_plan, benchmark_seconds, markers);
                if (actions.actual_mode != Mode::Unavailable) actions.map_marker_count = markers.size();
                ImGui::TextDisabled("%s", desk.show_markers ? "位置：本地样例" : "位置：本地样例（棋子已隐藏）");
                if (actions.actual_mode != desk.requested_mode) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s · %s", MapName(actions.actual_mode), RoboGo::arena::Status());
                }
            }
            ImGui::EndChild();
            ImGui::SameLine();
            if (ImGui::BeginChild("队友视频", {0.f,0.f}, ImGuiChildFlags_None, kNoScroll)) {
                const ImVec2 room = ImGui::GetContentRegionAvail();
                const float h = std::max(1.f,(room.y - 2.f * ImGui::GetStyle().ItemSpacing.y) / 3.f);
                for (int slot = 0; slot < static_cast<int>(kSeats.size()); ++slot)
                    DrawVideo(desk, feeds, slot, {std::max(1.f,room.x), h}, actions, clicked);
            }
            ImGui::EndChild();
        }
        if (clicked >= 0) desk.focused = desk.focused == clicked ? -1 : clicked;
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
    return actions;
}
