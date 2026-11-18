#pragma once
#include "app/application_types.h"
#include <algorithm>
#include <string_view>

namespace RoboGo::app::ul_drawing {
inline constexpr ImU32 Ink = IM_COL32(33, 55, 62, 255);
inline void Gap(float height) { ImGui::Dummy(ImVec2(0, height)); }
inline void Kicker(const char *text) {
    ImGui::TextColored(ImVec4(.41f,.82f,.72f,1), "%s", text);
}
inline void Title(const char *text, float size) {
    ImGui::PushFont(nullptr, size); ImGui::TextUnformatted(text); ImGui::PopFont();
}
inline void Status(std::string_view text) {
    if (text.empty()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,.80f,.51f,1));
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text.data(), text.data()+text.size());
    ImGui::PopTextWrapPos(); ImGui::PopStyleColor();
}
inline bool Button(const char *text, bool primary=false, float width=0) {
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.13f,.44f,.39f,1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(.17f,.53f,.47f,1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(.11f,.36f,.32f,1));
    }
    const bool pressed=ImGui::Button(text,ImVec2(width,0));
    if(primary) ImGui::PopStyleColor(3);
    return pressed;
}
struct Rect { ImVec2 a,b; };
inline Rect Fit(ImVec2 a, ImVec2 b, float ratio){
    const float w=std::min(b.x-a.x,(b.y-a.y)*ratio), h=w/ratio;
    const ImVec2 p((a.x+b.x-w)*.5f,(a.y+b.y-h)*.5f);
    return {p,ImVec2(p.x+w,p.y+h)};
}
inline void Label(ImDrawList *dl, ImVec2 point, float fontsize, const char *text, ImU32 color){
    ImFont *font=ImGui::GetFont();
    const auto size=font->CalcTextSizeA(fontsize,1e5f,0,text);
    dl->AddText(font,fontsize,ImVec2(point.x-size.x*.5f,point.y-size.y*.5f),color,text);
}
inline void Map(const TextureView &tex, float height, bool blue, bool points){
    const auto a=ImGui::GetCursorScreenPos();
    const float w=std::max(1.f,ImGui::GetContentRegionAvail().x); const ImVec2 b(a.x+w,a.y+height);
    ImGui::Dummy(ImVec2(w,height));
    auto *dl=ImGui::GetWindowDrawList();
    const auto box=Fit(a,b,1.5f);
    if (!tex.IsReady()) {
        Label(dl,ImVec2((a.x+b.x)*.5f,(a.y+b.y)*.5f),ImGui::GetFontSize(),"正在加载地图",IM_COL32_WHITE);
        return;
    }
    const float k=(box.b.x-box.a.x)/1200.f;
    const auto pos=[&](float x,float y){return ImVec2(box.a.x+(blue?1200-x:x)*k,box.a.y+(blue?800-y:y)*k);};
    // The background rotates; text is drawn again at its transformed anchor and stays upright.
    dl->AddImage(tex.texture_ref,box.a,box.b,blue?ImVec2(1,1):ImVec2(0,0),blue?ImVec2(0,0):ImVec2(1,1));
    dl->PushClipRect(box.a,box.b,true);
    const float font = ImGui::GetFontSize();
    // Scale with the active HUD font. Short names handle a narrow map without shrinking all labels.
    const float f = std::clamp(42.f * k, font * .95f, font * 1.28f);
    const float map_width = box.b.x - box.a.x;
    const bool compact = map_width < 20.f * f;
    const bool tiny = map_width < 13.f * f;
    const auto label = [&](float x, float y, float size, const char *text, ImU32 color) {
        const auto glyphs = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.f, text);
        if (glyphs.x > map_width || glyphs.y > box.b.y - box.a.y) return;
        const auto anchor = pos(x, y);
        const ImVec2 start(std::clamp(anchor.x - glyphs.x * .5f, box.a.x, box.b.x - glyphs.x),
                           std::clamp(anchor.y - glyphs.y * .5f, box.a.y, box.b.y - glyphs.y));
        dl->AddText(ImGui::GetFont(), size, start, color, text);
    };
    if (tiny) {
        // At this size label the three control areas and leave the terrain visible.
        const float area_font = std::min(f, 160.f * k);
        label(350, 192, area_font, "红高地", IM_COL32(140,54,42,255));
        label(600, 400, area_font, "中央", Ink);
        label(850, 617, area_font, "蓝高地", IM_COL32(43,77,126,255));
    } else {
        label(125, 108, f, compact ? "红补给" : "红方补给区", IM_COL32(140,54,42,255));
        label(1075, 700, f, compact ? "蓝补给" : "蓝方补给区", IM_COL32(43,77,126,255));
        label(600, 400, f, compact ? "中央" : "中央控制区", Ink);
        label(350, compact ? 210.f : 192.f, f, "高地", Ink);
        label(850, compact ? 599.f : 617.f, f, "高地", Ink);
        label(350, 320, f*.91f, "控制区", Ink);
        label(850, 480, f*.91f, "控制区", Ink);
        label(545, 48, f*.91f, compact ? "隧道" : "隧道位置", Ink);
        label(655, 752, f*.91f, compact ? "隧道" : "隧道位置", Ink);
    }
    if(points){
        const auto dot=[&](float x,float y,const char *label){
            const auto p=pos(x,y);
            dl->AddCircleFilled(p,std::max(8.f,f*.75f),IM_COL32(71,73,139,255));
            Label(dl,p,f,label,IM_COL32_WHITE);
        };
        dot(210,610,"1");dot(950,210,"2");
    }
    dl->PopClipRect();
}
} // namespace RoboGo::app::ul_drawing
