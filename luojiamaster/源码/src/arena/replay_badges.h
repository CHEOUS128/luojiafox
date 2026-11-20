#pragma once

#include "arena/replay_projection.h"
#include "arena/replay_marker.h"
#include "arena/replay_piece.h"
#include "imgui.h"
#include <array>
#include <cstdio>
#include <limits>
#include <vector>

namespace RoboGo::arena {
namespace badges {

struct Box { ImVec2 lo, hi; };

inline float Overlap(const Box& a, const Box& b) {
    const float w = std::min(a.hi.x, b.hi.x) - std::max(a.lo.x, b.lo.x);
    const float h = std::min(a.hi.y, b.hi.y) - std::max(a.lo.y, b.lo.y);
    return std::max(w, 0.f) * std::max(h, 0.f);
}

inline Box Place(const Box& piece, ImVec2 size, const Box& canvas, const Box* used, int count) {
    Box best{};
    float min_overlap = std::numeric_limits<float>::max();
    const float gap = size.y * .24f;
    const float mid_y = (piece.lo.y + piece.hi.y) * .5f;
    // Try either side, then nearby rows. Keep the small foot on the real
    // sample point even when nearby labels have to move away from each other.
    for (int row : {0, -1, 1, -2, 2, -3, 3, -4, 4}) {
        for (bool left : {false, true}) {
            const float x = std::clamp(left ? piece.lo.x-gap-size.x : piece.hi.x+gap, canvas.lo.x, canvas.hi.x-size.x);
            const float y = std::clamp(mid_y - size.y*.5f + row*(size.y+2.f), canvas.lo.y, canvas.hi.y-size.y);
            const Box candidate{{x,y}, {x+size.x,y+size.y}};
            float overlap = 0;
            for (int i=0; i<count; ++i) overlap += Overlap(candidate,used[i]);
            if (!overlap) return candidate;
            if (overlap < min_overlap) { best=candidate; min_overlap=overlap; }
        }
    }
    return best;
}

template<class Project,class ObserveLabel>
void Draw(ImDrawList* draw, const std::vector<ReplayMarker>& markers,
          const Box& canvas, Project project, bool flat, ObserveLabel observe_label) {
    const float font = ImGui::GetFontSize();
    std::array<Box,16> occupied; // all visible pieces, then their name plates
    std::array<const ReplayMarker*,8> visible{};
    int count=0;
    for(const auto& m:markers) {
        if(count==visible.size()) break;
        const auto& model=pieces::Models()[pieces::Index(m.number)];
        const auto anchor=project(m.u,m.v,m.floor_y);
        Box bounds{anchor,anchor};
        const auto local=[&](float x,float z,float h) {return project(m.u+x/28.f,m.v+z/15.f,m.floor_y+h);};
        std::array<ImVec2,32> outline{};
        const auto tint=pieces::Tint(m.team,m.age_seconds>=3);
        const auto fill=[&](float shade) {
            return ImGui::ColorConvertFloat4ToU32({tint.r*shade,tint.g*shade,tint.b*shade,1});
        };
        for(std::size_t i=0;i<model.outline.size();++i) {
            const auto p=model.outline[i];
            outline[i]=local(p.x,p.z,0);
            for(const auto pt:{outline[i],local(p.x,p.z,model.height)}) {
                bounds.lo={std::min(bounds.lo.x,pt.x),std::min(bounds.lo.y,pt.y)};
                bounds.hi={std::max(bounds.hi.x,pt.x),std::max(bounds.hi.y,pt.y)};
            }
        }
        // Keep a partly visible piece and its name until its last corner leaves.
        if(Overlap(bounds,canvas)<=0.f) continue;
        if(flat) {
            const int n=static_cast<int>(model.outline.size());
            draw->AddConvexPolyFilled(outline.data(),n,fill(.80f));
            for(int i=0;i<n;++i) outline[i]=local(.95f*model.outline[i].x,.95f*model.outline[i].z,0);
            draw->AddConvexPolyFilled(outline.data(),n,fill(.71f));
            const auto span=pieces::InkSize(model,1,0);
            const auto lo=local(-span.x*.5f,-span.z*.5f,0), hi=local(span.x*.5f,span.z*.5f,0);
            // Choose the nearest atlas size so the small full-field labels keep
            // the font's real strokes. Map the ink rectangle onto the top face.
            const float raster=std::clamp(std::round((hi.y-lo.y)*1.25f),10.f,72.f);
            auto* baked=ImGui::GetFont()->GetFontBaked(raster);
            if(auto* glyph=baked->FindGlyphNoFallback(static_cast<ImWchar>(pieces::RoleCharacter(m.number)))) {
                const ImU32 ink=m.age_seconds>=3?IM_COL32(191,196,201,255):IM_COL32(254,249,238,255);
                draw->AddImage(ImGui::GetIO().Fonts->TexRef,lo,hi,
                               {glyph->U0,glyph->V0},{glyph->U1,glyph->V1},ink);
            }
        }
        occupied[count]=bounds;
        visible[count++]=&m;
    }
    int reserved=count;
    for(int i=0;i<count;++i) {
        const auto& m=*visible[i];
        const bool old = m.age_seconds >= 3;
        char label[48];
        if (old) std::snprintf(label,sizeof(label),"%s%d  %d 秒前",m.team ? "蓝" : "红",m.number,m.age_seconds);
        else std::snprintf(label,sizeof(label),"%s%d",m.team ? "蓝" : "红",m.number);
        const ImVec2 text = ImGui::CalcTextSize(label);
        const ImVec2 size{font*.65f + text.x, font*1.27f};
        if (size.x > canvas.hi.x-canvas.lo.x || size.y > canvas.hi.y-canvas.lo.y) continue;
        const Box& piece=occupied[i];
        const Box b = Place(piece,size,canvas,occupied.data(),reserved);
        occupied[reserved++] = b;
        observe_label(b,piece);
        const ImU32 color = old ? IM_COL32(174,180,188,200)
                            : m.team ? IM_COL32(93,212,255,255) : IM_COL32(255,124,116,255);
        const ImVec2 mid{(piece.lo.x+piece.hi.x)*.5f,(piece.lo.y+piece.hi.y)*.5f};
        const ImVec2 pin{std::clamp(mid.x,b.lo.x,b.hi.x),std::clamp(mid.y,b.lo.y,b.hi.y)};
        draw->AddLine({std::clamp(pin.x,piece.lo.x,piece.hi.x),std::clamp(pin.y,piece.lo.y,piece.hi.y)},pin,color,1);
        const float rounding = size.y*.24f;
        draw->AddRectFilled(b.lo,b.hi,old ? IM_COL32(21,26,32,183) : IM_COL32(16,23,31,235),rounding);
        draw->AddRect(b.lo,b.hi,color,rounding);
        draw->AddText({b.lo.x+font*.325f,b.lo.y+(size.y-text.y)*.5f},
                      old ? IM_COL32(210,214,219,218) : IM_COL32_WHITE,label);
    }
}

} // namespace badges
} // namespace RoboGo::arena
