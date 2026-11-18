#pragma once

#include "app/ul_drawing.h"
#include <cstdint>
#include <string_view>

namespace RoboGo::app::window_checks {

enum class Lane { None, Official, Camera, Teammate, Map };
enum class Cmd { None, Start, Stop, Back };
struct State {
    bool ul=true, blue=false;
    Lane selected=Lane::None;
};

inline const char* Name(Lane lane) {
    switch(lane) {
        case Lane::Official: return "官方图传";
        case Lane::Camera: return "工业相机";
        case Lane::Teammate: return "队友小窗";
        case Lane::Map: return "场地图";
        default: return "";
    }
}

inline Cmd Choose(State &s,Lane target) {
    if(target==s.selected) {s.selected=Lane::None;return Cmd::Stop;}
    s.selected=target; s.blue=false;
    return (target==Lane::Official || target==Lane::Camera)? Cmd::Start : Cmd::Stop;
}

inline void VideoSurface(const TextureView &view,std::uint64_t count,float height,std::string_view status) {
    namespace ui=ul_drawing;
    const auto a=ImGui::GetCursorScreenPos();
    const float w=std::max(1.f,ImGui::GetContentRegionAvail().x);
    const ImVec2 b(a.x+w,a.y+height);
    ImGui::Dummy(ImVec2(w,height));
    auto *dl=ImGui::GetWindowDrawList();
    dl->AddRectFilled(a,b,IM_COL32(9,17,20,255),4);
    if(view.IsReady() && count>0 && view.size.x>0 && view.size.y>0) {
        const auto box=ui::Fit(a,b,view.size.x/view.size.y);
        dl->AddImage(view.texture_ref,box.a,box.b);
        return;
    }
    dl->PushClipRect(a,b,true);
    ui::Label(dl,ImVec2((a.x+b.x)*.5f,(a.y+b.y)*.5f),ImGui::GetFontSize(),
              status.empty() ? "正在等录像的第一帧" : "未出画，请看上方提示",IM_COL32(180,198,200,255));
    dl->PopClipRect();
}

inline void TeammateCard(float height) {
    namespace ui=ul_drawing;
    const auto a=ImGui::GetCursorScreenPos(); const float w=std::max(1.f,ImGui::GetContentRegionAvail().x);
    const ImVec2 b(a.x+w,a.y+height);
    ImGui::Dummy(ImVec2(w,height));
    auto *dl=ImGui::GetWindowDrawList(); dl->AddRectFilled(a,b,IM_COL32(10,17,19,255),4);
    const auto desk=ui::Fit(a,b,16.f/9.f);
    dl->AddRectFilled(desk.a,desk.b,IM_COL32(26,44,48,255));
    const float dw=desk.b.x-desk.a.x, dh=desk.b.y-desk.a.y;
    ui::Label(dl,{desk.a.x+dw*.46f,desk.a.y+dh*.5f},ImGui::GetFontSize(),"主画面区域",IM_COL32(177,198,200,255));
    // Same anchor and relative width as ShowTeammateViewport in the classic client.
    const ImVec2 origin(desk.a.x+dw*.8f,desk.a.y+dh*.15f);
    const ui::Rect box{origin,{desk.b.x,origin.y+dw*.2f*9.f/16.f}};
    const float cw=(box.b.x-box.a.x)/5.f;
    constexpr ImU32 bars[]={IM_COL32(66,88,91,255),IM_COL32(60,107,107,255),IM_COL32(73,132,124,255),
                           IM_COL32(99,148,129,255),IM_COL32(131,160,137,255)};
    for(int i=0;i<5;++i) dl->AddRectFilled({box.a.x+i*cw,box.a.y},{box.a.x+(i+1)*cw,box.b.y},bars[i]);
    const auto center=ImVec2((box.a.x+box.b.x)*.5f,(box.a.y+box.b.y)*.5f);
    dl->PushClipRect(box.a,box.b,true);
    ui::Label(dl,center,ImGui::GetFontSize(),"队友小窗",IM_COL32_WHITE);
    dl->PopClipRect();
}

// Draw inside the disconnected viewer. Only the two video tests request replay.
inline Cmd Draw(State &s,const TextureView &official,const TextureView &camera,const TextureView &map,
                std::uint64_t official_count,std::uint64_t camera_count,std::string_view status={}) {
    namespace ui=ul_drawing;
    if(ui::Button("返回")) return Cmd::Back;
    ImGui::SameLine(); ImGui::TextUnformatted(s.ul?"高校联盟赛 3V3  /  窗口自测":"超级对抗赛 7V7  /  基础版窗口自测");
    ImGui::Separator();
    ImGui::TextWrapped("选一个窗口查看效果。视频来自本机录像，不接车、不登录。");
    Cmd cmd=Cmd::None;
    constexpr Lane lanes[]={Lane::Official,Lane::Camera,Lane::Teammate,Lane::Map};
    const float width=ImGui::GetContentRegionAvail().x;
    const int cols= width>=600.f?4:width>=290.f?2:1;
    const float gap=ImGui::GetStyle().ItemSpacing.x;
    const float cell=std::max(1.f,(width-gap*(cols-1))/cols);
    for(int i=0;i<4;++i) {
        if(i%cols!=0) ImGui::SameLine();
        ImGui::PushID(i);
        if(ui::Button(Name(lanes[i]),s.selected==lanes[i],cell)) cmd=Choose(s,lanes[i]);
        ImGui::PopID();
    }
    ui::Gap(3);
    if(s.selected==Lane::None) {
        ImGui::TextDisabled("尚未开始测试。");
        return cmd;
    }
    ImGui::Text("%s   /   %s",Name(s.selected),
        (s.selected==Lane::Official || s.selected==Lane::Camera) ? "历史录流" :
        s.selected==Lane::Teammate ? "本机布局示意" : "本地地图资源");
    if(ui::Button("停止测试")) {s.selected=Lane::None; return Cmd::Stop;}
    if(s.selected==Lane::Official || s.selected==Lane::Camera) {
        ImGui::SameLine(); if(ui::Button("从头播放")) cmd=Cmd::Start;
        ui::Status(status);
        const bool is_official=s.selected==Lane::Official;
        // Draw no leftover frame on the click that restarts a decoder.
        const auto n=cmd==Cmd::Start?std::uint64_t{0}:(is_official?official_count:camera_count);
        const auto &view=is_official?official:camera;
        if(view.IsReady() && n>0) ImGui::Text("录像 %d × %d   /   已更新 %llu 帧",
            static_cast<int>(view.size.x),static_cast<int>(view.size.y),static_cast<unsigned long long>(n));
        VideoSurface(view,n,std::max(100.f,ImGui::GetContentRegionAvail().y-6.f),status);
    } else if(s.selected==Lane::Map) {
        ui::Status(status);
        if(!map.IsReady()) { ImGui::TextUnformatted("地图资源尚未就绪。"); return cmd; }
        if(s.ul) {
            ImGui::SameLine(); if(ui::Button(s.blue?"蓝方视角 / 翻转":"红方视角 / 翻转")) s.blue=!s.blue;
            ImGui::TextUnformatted("2027 联盟赛 3V3 场地");
            ui::Map(map,std::min(std::max(100.f,ImGui::GetContentRegionAvail().y-6.f),width/1.5f),s.blue,false);
        } else {
            ImGui::TextWrapped("显示基础客户端沿用的地图，用于查资源和显示比例；不是 2027 赛场新底图。");
            const auto a=ImGui::GetCursorScreenPos();
            const float h=std::max(100.f,ImGui::GetContentRegionAvail().y-6.f); const ImVec2 b(a.x+width,a.y+h);
            auto box=ui::Fit(a,b,280.f/150.f);
            ImGui::Dummy(ImVec2(width,h)); ImGui::GetWindowDrawList()->AddImage(map.texture_ref,box.a,box.b,{45.f/map.size.x,31.f/map.size.y},{1226.f/map.size.x,683.f/map.size.y});
        }
    } else {
        ImGui::TextWrapped("右上角色块是测试卡，宽度约占主画面五分之一。这里只检查布局，不连接队友。");
        TeammateCard(std::max(100.f,ImGui::GetContentRegionAvail().y-6.f));
    }
    return cmd;
}
} // namespace RoboGo::app::window_checks
