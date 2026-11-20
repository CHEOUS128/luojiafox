#pragma once

#include "app/application_types.h"
#include "app/ul_drawing.h"
#include "panel/brand_mark.h"
#include "app/window_checks.h"
#include <algorithm>
#include <cstdint>
#include <string_view>

namespace RoboGo::app::ul_preview {

enum class Page { Welcome, Roles, Desk, WindowChecks };
enum class Action { None, Start, Restart, Back, Stop, Quit };
struct UiState {
    Page page = Page::Welcome, previous = Page::Roles;
    bool dedicated_check = false;
    window_checks::State checks{};
    void OpenChecks(bool ul) { previous=page; checks={}; checks.ul=ul; page=Page::WindowChecks; }
    bool heavy = false, blue = false, industrial = false, sample_points = false;
};

namespace detail {
using namespace RoboGo::app::ul_drawing;
inline void Video(const TextureView &tex, bool has_frames, float height, bool failed){
    const auto a=ImGui::GetCursorScreenPos();
    const float width=std::max(1.f,ImGui::GetContentRegionAvail().x);
    const ImVec2 b(a.x+width,a.y+height); auto *dl=ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(width,height)); dl->AddRectFilled(a,b,IM_COL32(9,16,19,255),4);
    if (has_frames && tex.IsReady()) {
        const auto box=Fit(a,b,tex.size.x/tex.size.y);
        dl->AddImage(tex.texture_ref,box.a,box.b);
    } else {
        dl->PushClipRect(a,b,true);
        Label(dl,ImVec2((a.x+b.x)*.5f,(a.y+b.y)*.5f),ImGui::GetFontSize(),
              failed?"回放未就绪，请看上方提示":"等待本路的第一帧",IM_COL32(167,183,185,255));
        dl->PopClipRect();
    }
}
} // namespace detail

inline Action Draw(UiState &s, const TextureView &udp, const TextureView &cbb,
                   const TextureView &map, std::uint64_t udp_count, std::uint64_t cbb_count,
                   std::string_view status={}) {
    using namespace detail;
    const auto *vp=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos); ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(.073f,.105f,.118f,1));
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.16f,.22f,.24f,1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(.21f,.31f,.32f,1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(.17f,.26f,.27f,1));
    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.92f,.94f,.93f,1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(20,16));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(12,7));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,5.f);
    ImGui::Begin("珞珈狐  /  LUOJIAFOX###ul-offline",nullptr,
        ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
    const float all=ImGui::GetContentRegionAvail().x;
    Action out=Action::None;
    if(s.page==Page::WindowChecks) {
        switch(window_checks::Draw(s.checks,udp,cbb,map,udp_count,cbb_count,status)){
            case window_checks::Cmd::Start: out=Action::Start; break;
            case window_checks::Cmd::Stop: out=Action::Stop; break;
            case window_checks::Cmd::Back:
                s.page=s.previous; out=s.dedicated_check?Action::Quit:Action::Back; break;
            case window_checks::Cmd::None: break;
        }
    } else if(s.page!=Page::Desk) {
        Gap(std::clamp(vp->WorkSize.y*.08f,4.f,58.f));
        const float block=std::min(640.f,all); ImGui::Indent(std::max(1.f,(all-block)*.5f));
        luojiamaster::brand::Masthead(s.page == Page::Welcome);
        Gap(9); Title(s.page==Page::Welcome?"欢迎使用":"离线体验",std::clamp(all*.05f,22.f,32.f));
        Gap(4); ImGui::TextUnformatted(s.page==Page::Welcome?"自定义客户端    /    赛事入口":"无需登录    /    本机测试");
        Gap(16); Status(status);
        if(s.page==Page::Welcome) {
            if(Button("进入离线体验",true,block)) { s=UiState{}; s.page=Page::Roles; }
            Gap(12);
            if(Button("退出客户端")) out=Action::Quit;
        } else {
            ImGui::TextUnformatted("检查单个窗口");
            if(Button("3V3 窗口自检",false,block)) s.OpenChecks(true);
            if(Button("7V7 窗口自检",false,block)) s.OpenChecks(false);
            Gap(12); ImGui::TextUnformatted("高校联盟赛 3V3   /   双路录流体验");
            Gap(10);
            const auto start=[&](bool heavy){s=UiState{}; s.page=Page::Desk; s.heavy=heavy; s.industrial=heavy; return Action::Start;};
            if(Button("步兵  3 号     /     先看官方图传",true,block)) out=start(false);
            if(Button("重装  1 号     /     先看工业相机",false,block)) out=start(true);
            Gap(8); if(Button("返回赛事入口")) s.page=Page::Welcome;
        }
        ImGui::Unindent(std::max(1.f,(all-block)*.5f));
    } else {
        Kicker("LUOJIAFOX     /     联盟赛 3V3     /     离线工作台");
        Title(s.heavy?"重装  1 号":"步兵  3 号",26); Status(status);
        if(Button(s.industrial?"主画面：工业相机   /   切换":"主画面：官方图传   /   切换",true)) s.industrial=!s.industrial;
        if (all>510) ImGui::SameLine();
        if(Button("从头回放")) out=Action::Restart;
        ImGui::SameLine(); if(Button("返回选岗")) {s=UiState{}; s.page=Page::Roles; out=Action::Back;}
        Gap(3);
        const bool wide=all>=760.f;
        const float spacing=ImGui::GetStyle().ItemSpacing.x;
        const float left=wide?(all-spacing)*.55f:all;
        const float right=wide?std::max(1.f,all-spacing-left):all;
        const float remaining=std::max(240.f, ImGui::GetContentRegionAvail().y);
        const char *main=s.industrial?"工业相机":"官方图传";
        const char *other=s.industrial?"官方图传":"工业相机";
        ImGui::BeginChild("主画面",ImVec2(left,wide?remaining:std::clamp(left*.6f,210.f,360.f)),ImGuiChildFlags_Borders);
        ImGui::Text("%s   /   历史录流",main);
        Video(s.industrial?cbb:udp,(s.industrial?cbb_count:udp_count)>0,
              std::max(30.f,ImGui::GetContentRegionAvail().y),!status.empty());
        ImGui::EndChild(); if(wide) ImGui::SameLine();
        const float h=wide?remaining:std::clamp(right*.92f,410.f,650.f);
        ImGui::BeginChild("场地图和副画面",ImVec2(right,h),ImGuiChildFlags_Borders);
        ImGui::TextUnformatted("2027 场地图   /   12 × 8 米");
        if(Button(s.blue?"蓝方视角   /   翻转":"红方视角   /   翻转")) s.blue=!s.blue;
        if(right>260) ImGui::SameLine();
        ImGui::Checkbox("样例点",&s.sample_points);
        const float inner=std::max(1.f,ImGui::GetContentRegionAvail().x);
        const float rest=std::max(100.f,ImGui::GetContentRegionAvail().y);
        Map(map,std::min(inner*2.f/3.f,rest*.59f),s.blue,s.sample_points);
        if(s.sample_points) ImGui::TextDisabled("1、2 是离线样例点，不是车辆位置。");
        ImGui::Separator(); ImGui::Text("%s   /   历史录流",other);
        Video(s.industrial?udp:cbb,(s.industrial?udp_count:cbb_count)>0,
              std::max(48.f,ImGui::GetContentRegionAvail().y),!status.empty());
        ImGui::EndChild();
    }
    ImGui::End(); ImGui::PopStyleVar(3); ImGui::PopStyleColor(5);
    return out;
}
} // namespace RoboGo::app::ul_preview
