#include "panel/cup_map_panel.h"
#include "panel/replay_video_cards.h"
#include "app/ul_drawing.h"
#include "net/teammate_link.h"

namespace {
using Time = CupMapReplay::Time;
using Canvas = RoboGo::arena::ReplayCanvas;
using namespace luojiamaster::replay_cards;

void DrawReplayPieces(CupMapReplay& replay, const ImRect& field, bool cup_2027, Time now, Canvas* canvas) {
    const auto peers = CupMapReplay::PeerSeats(replay.seat);
    const auto font = ImGui::GetFontSize();
    if (field.GetWidth() < 2.f || field.GetHeight() < 2.f) return;
    const auto radius = canvas ? font * .72f : std::clamp(field.GetWidth() * .018f, 3.f, font * .62f);
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(field.Min,field.Max,true);
    for (const auto& piece : replay.Visible(now)) {
        const auto uv = CupMapReplay::FigureUV(piece.u,piece.v,cup_2027,replay.seat > 100);
        const ImVec2 p{field.Min.x + uv.x*field.GetWidth(), field.Min.y + uv.y*field.GetHeight()};
        if (!field.Contains(p)) continue;
        const int id = piece.team * 100 + piece.number;
        const bool stale = piece.age_seconds >= 3;
        const ImU32 tint = stale ? IM_COL32(128,137,144,220)
                                : piece.team ? IM_COL32(45,143,216,255) : IM_COL32(201,71,69,255);
        draw->AddCircleFilled(p,radius+1.5f,IM_COL32(19,27,33,230));
        draw->AddCircleFilled(p,radius,tint);
        if (id == replay.seat)
            draw->AddCircle(p,radius+2.f,IM_COL32(255,234,155,255),0,2.f);

        const char* number = id == replay.seat ? "我" : piece.number == 1 ? "1" : piece.number == 3 ? "3"
                                                                                     : piece.number == 4 ? "4" : "7";
        if (canvas || id == replay.seat || radius >= 5.f) {
            const float size = canvas ? font : std::max(8.f,radius*1.55f);
            const auto measured = ImGui::GetFont()->CalcTextSizeA(size,FLT_MAX,0.f,number);
            draw->AddText(ImGui::GetFont(),size,{p.x-measured.x*.5f,p.y-measured.y*.5f},IM_COL32_WHITE,number);
        }
        if (!canvas) continue;
        for (int s=0; s<2; ++s)
            if (id == peers[s]) canvas->pins[s] = {p,true,true,piece.age_seconds};

        char label[32];
        if (stale) std::snprintf(label,sizeof(label),"%s%d · %d 秒前",piece.team?"蓝":"红",piece.number,piece.age_seconds);
        else std::snprintf(label,sizeof(label),"%s%d",piece.team?"蓝":"红",piece.number);
        const ImVec2 size = ImGui::CalcTextSize(label);
        const float pad = font*.24f;
        const float x = std::clamp(p.x+radius+pad*2.f,field.Min.x,std::max(field.Min.x,field.Max.x-size.x-2.f*pad));
        const float y = std::clamp(p.y-size.y*.5f-pad,field.Min.y,std::max(field.Min.y,field.Max.y-size.y-2.f*pad));
        const ImRect tag({x,y},{x+size.x+2.f*pad,y+size.y+2.f*pad});
        draw->AddRectFilled(tag.Min,tag.Max,IM_COL32(17,26,34,232),pad);
        draw->AddText({x+pad,y+pad},stale?IM_COL32(192,199,202,255):IM_COL32_WHITE,label);
        if (canvas->label_count < static_cast<int>(canvas->labels.size()))
            canvas->labels[canvas->label_count++] = {tag.Min,tag.Max,{p.x-radius,p.y-radius},{p.x+radius,p.y+radius}};
    }
    draw->PopClipRect();
}

void LocalSignalMenu(CupMapReplay& replay, const RecordingSlot (&slots)[2], Time now, MainWindowActions& actions) {
    if (ImGui::Button("测试信号")) ImGui::OpenPopup("##cup-signals");
    if (!ImGui::BeginPopup("##cup-signals")) return;
    const auto peers = CupMapReplay::PeerSeats(replay.seat);
    for (int i=0; i<2; ++i) {
        ImGui::PushID(i);
        const std::string pause = std::string(replay.PositionPaused(peers[i])?"恢复位置 · ":"暂停位置 · ") + slots[i].label;
        if (ImGui::MenuItem(pause.c_str())) replay.TogglePosition(peers[i],now);
        const bool stopped = slots[i].picture && slots[i].picture->paused;
        const std::string stop = std::string(stopped?"恢复录像 · ":"断开录像 · ") + slots[i].label;
        if (ImGui::MenuItem(stop.c_str(),nullptr,false,slots[i].source>=0)) actions.toggle_replay_feed=slots[i].source;
        ImGui::PopID();
    }
    ImGui::EndPopup();
}
} // namespace

void DrawCupReplayMini(CupMapReplay& local_replay, const ImRect& field, bool cup_2027) {
    DrawReplayPieces(local_replay,field,cup_2027,CupMapReplay::Samples::Clock::now(),nullptr);
}

MainWindowActions ShowCupExpandedMap(ImTextureRef texture, ImVec2 size, bool cup_2027, bool blue,
                                     CupMapReplay* replay, const TeammateViewState* recordings) {
    MainWindowActions actions{};
    const auto* vp = ImGui::GetMainViewport();
    const float base = ImGui::GetFontSize();
    const float font = std::clamp(2.f*std::floor(std::min(vp->WorkSize.x/96.f,vp->WorkSize.y/55.f)/2.f),
                                  base,std::max(36.f,base));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * font / base);
    const float pad = ImGui::GetFontSize()*.7f;
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{pad,pad});
    ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(.049f,.074f,.088f,1.f));
    const auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("超级对抗赛全场图###CupExpandedMap",nullptr,flags)) {
        const bool local = replay && recordings;
        const auto now = CupMapReplay::Samples::Clock::now();
        const auto competition = cup_2027 ? RoboGo::app::Competition::Uc2027 : RoboGo::app::Competition::Uc2026;
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%d 超级对抗赛 · %s方",cup_2027?2027:2026,blue?"蓝":"红");
        RecordingSlot slots[2]{};
        if (local) {
            const auto peers = CupMapReplay::PeerSeats(replay->seat);
            for (int i=0; i<2; ++i) {
                const int source = ReplaySlotFor(*recordings,peers[i]);
                slots[i] = {RoboGo::net::TeamSeatName(competition,peers[i]),source<0?nullptr:&recordings->peers[source],source};
            }
            ImGui::SameLine(0,pad);
            ImGui::TextDisabled("本地测试 · 位置：本地样例");
            ImGui::SameLine(0,pad);
            LocalSignalMenu(*replay,slots,now,actions);
        }
        const char* hint = "M / Esc 收起";
        const float hint_x = ImGui::GetWindowWidth()-pad-ImGui::CalcTextSize(hint).x;
        if (ImGui::GetItemRectMax().x+pad < ImGui::GetWindowPos().x+hint_x)
            ImGui::SameLine(hint_x);
        ImGui::TextDisabled("%s",hint);
        ImGui::Separator();
        const auto lo = ImGui::GetCursorScreenPos();
        const auto area = ImGui::GetContentRegionAvail();
        Canvas canvas{};
        canvas.lo = lo;
        canvas.hi = {lo.x+std::max(1.f,area.x),lo.y+std::max(1.f,area.y)};
        if (texture.GetTexID()!=ImTextureID_Invalid && size.x>0.f && size.y>0.f) {
            const auto [a,b] = cup_2027 ? std::pair{blue?ImVec2(1,1):ImVec2(0,0),blue?ImVec2(0,0):ImVec2(1,1)}
                                         : LegacyMapFieldUV(size);
            const float aspect = std::abs((b.x-a.x)*size.x) / std::max(1.f,std::abs((b.y-a.y)*size.y));
            const auto box = RoboGo::app::ul_drawing::Fit(canvas.lo,canvas.hi,std::max(.01f,aspect));
            ImGui::GetWindowDrawList()->AddImage(texture,box.a,box.b,a,b);
            if (local) DrawReplayPieces(*replay,{box.a,box.b},cup_2027,now,&canvas);
        } else ImGui::TextDisabled("场馆图未就绪");
        if (local) {
            const auto& io = ImGui::GetIO();
            const int hit = ImGui::IsWindowHovered() ? replay->cards.Hit(io.MousePos,replay->focused,2) : -1;
            int clicked = -1;
            DrawRecordingCards(replay->cards,replay->focused,replay->zoom,slots,2,canvas,true,true,hit,
                               clicked,actions.map_drawn_mask);
            if (clicked>=0) replay->focused = replay->focused==clicked ? -1 : clicked;
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::PopFont();
    return actions;
}
