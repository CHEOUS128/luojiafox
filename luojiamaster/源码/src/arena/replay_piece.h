#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace RoboGo::arena::pieces {

// All three footprints fit the same 60 cm circle in the local arena.
inline constexpr float radius = .30f;
struct Point { float x, z; };
struct Vertex { float x, y, z, shade; };
struct Model {
    std::vector<Point> outline;
    std::vector<Vertex> triangles;
    float height{};
};

inline Model MakeModel(int number) {
    constexpr float pi = 3.14159265f;
    Model m;
    m.height = number == 1 ? .36f : (number == 7 ? .29f : .23f);
    if (number == 1) {
        constexpr float corner = .165f;
        constexpr float rounding = radius - 1.41421356f * corner;
        for (int q = 0; q < 4; ++q) {
            const float cx = q == 0 || q == 3 ? corner : -corner;
            const float cz = q < 2 ? corner : -corner;
            for (int i = 0; i <= 4; ++i) {
                const float a = (q + i / 4.f) * pi * .5f;
                m.outline.push_back({cx + rounding * std::cos(a), cz + rounding * std::sin(a)});
            }
        }
    } else {
        const int count = number == 7 ? 6 : 32;
        for (int i = 0; i < count; ++i) {
            const float a = i * (2 * pi / count);
            m.outline.push_back({radius * std::cos(a), radius * std::sin(a)});
        }
    }
    const auto vertex = [](Point p, float scale, float y, float shade) {
        return Vertex{p.x * scale, y, p.z * scale, shade};
    };
    const auto triangle = [&](Vertex a, Vertex b, Vertex c) {
        m.triangles.insert(m.triangles.end(), {a,b,c});
    };
    for (std::size_t i = 0; i < m.outline.size(); ++i) {
        Point a = m.outline[i], b = m.outline[(i+1) % m.outline.size()];
        const float dx = b.x - a.x, dz = b.z - a.z;
        const float diffuse = std::max(0.f, (.60f * dx - .80f * dz) / std::hypot(dx,dz));
        const auto wall = [&](float bottom, float r0, float top, float r1, float shade) {
            const auto ba=vertex(a,r0,bottom,shade), bb=vertex(b,r0,bottom,shade);
            const auto ta=vertex(a,r1,top,shade), tb=vertex(b,r1,top,shade);
            triangle(tb,ta,ba); triangle(tb,ba,bb);
        };
        wall(0,.94f,.035f,1,.48f + .12f*diffuse);
        wall(.035f,1,m.height-.045f,.99f,.60f + .25f*diffuse);
        wall(m.height-.045f,.99f,m.height,.95f,.78f + .20f*diffuse);
        triangle({0,m.height,0,1},vertex(a,.95f,m.height,1),vertex(b,.95f,m.height,1));
    }
    return m;
}

inline const std::array<Model,3>& Models() {
    static const std::array<Model,3> models{MakeModel(1), MakeModel(3), MakeModel(7)};
    return models;
}
inline std::size_t Index(int number) { return number == 1 ? 0 : (number == 7 ? 2 : 1); }

inline char32_t RoleCharacter(int number) {
    return number == 1 ? U'重' : (number == 7 ? U'哨' : U'步');
}

// Fit an upright 1.13:1 CJK ink rectangle into the bevel's inner polygon
// even when the field turns. Sizes below are measured in local arena metres.
inline Point InkSize(const Model& model,float co,float si) {
    constexpr float aspect = 1.13f;
    float half_width = .225f;
    for(std::size_t i=0;i<model.outline.size();++i) {
        Point a=model.outline[i], b=model.outline[(i+1)%model.outline.size()];
        const float nx=b.z-a.z, nz=a.x-b.x;
        const float support=std::abs(nx*co-nz*si) + std::abs(nx*si+nz*co)/aspect;
        if(support>0) half_width=std::min(half_width,.92f*(nx*a.x+nz*a.z)/support);
    }
    return {2*half_width,2*half_width/aspect};
}

struct Color { float r,g,b; };
inline Color Tint(int team, bool stale) {
    if (stale) return {.43f,.45f,.48f};
    return team ? Color{.27f,.63f,.81f} : Color{.85f,.40f,.35f};
}

} // namespace RoboGo::arena::pieces
