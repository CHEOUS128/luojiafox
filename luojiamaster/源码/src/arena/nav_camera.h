#pragma once

#include <algorithm>
#include <cmath>

namespace RoboGo::arena {

struct NavPoint { float x{}, y{}; };
struct NavWorld { float x{}, y{}, z{}; };

// The pivot is a point on the arena. Anchor is its position in the canvas
// (0..1 from the top left, possibly outside after panning). Keeping these
// separate lets us orbit an off-centre platform without first centring it.
struct NavCamera {
    float yaw{}, tilt{}, zoom{1.f};
    NavWorld pivot{};
    NavPoint anchor{.5f, .5f};

    static float PixelsPerMetre(float width, float height, float scale) {
        return .93f * std::min(width / 28.f, height / 16.01f) * scale;
    }

    float FlatScale(float width, float height) const {
        return std::min(width / 903.f, height / 499.f) * zoom;
    }

    // Calibration of the field plane to the original 903 x 499 rule figure.
    static constexpr float figure_x0 = 14.7967f, figure_y0 = 15.6429f;
    static constexpr float figure_u_x = 869.8846f, figure_v_x = 2.5220f;
    static constexpr float figure_v_y = 466.7143f;

    static NavPoint FigurePoint(NavWorld world) {
        const float u = .5f + world.x / 28.f;
        const float v = .5f + world.z / 15.f;
        return {figure_x0 + figure_u_x * u + figure_v_x * v,
                figure_y0 + figure_v_y * v};
    }

    NavPoint FlatOrigin(float width, float height) const {
        const auto figure = FigurePoint(pivot);
        const float scale = FlatScale(width, height);
        return {width * anchor.x - scale * figure.x,
                height * anchor.y - scale * figure.y};
    }

    NavPoint Project(float width, float height, bool flat, NavWorld world) const {
        if (flat) {
            const auto start = FlatOrigin(width, height);
            const auto figure = FigurePoint(world);
            const float scale = FlatScale(width, height);
            return {start.x + scale * figure.x, start.y + scale * figure.y};
        }
        const float x = world.x - pivot.x, z = world.z - pivot.z;
        const float co = std::cos(yaw), si = std::sin(yaw);
        const float right = x * co - z * si;
        const float down = (x * si + z * co) * std::cos(tilt)
                         - (world.y - pivot.y) * std::sin(tilt);
        const float scale = PixelsPerMetre(width, height, zoom);
        return {width * anchor.x + scale * right, height * anchor.y + scale * down};
    }

    // Shader clip coordinates have a positive-up Y axis, unlike the canvas.
    NavPoint OffsetNdc(float width, float height) const {
        if (width <= 0.f || height <= 0.f) return {};
        const auto origin = Project(width, height, false, {});
        return {2.f * origin.x / width - 1.f, 1.f - 2.f * origin.y / height};
    }

    NavWorld PickAtHeight(float width, float height, bool flat, NavPoint pixel,
                          float world_y = 0.f) const {
        if (width <= 0.f || height <= 0.f) return pivot;
        const float dx = pixel.x - width * anchor.x, dy = pixel.y - height * anchor.y;
        if (flat) {
            const float scale = FlatScale(width, height);
            const float dv = dy / (scale * figure_v_y);
            const float du = (dx / scale - figure_v_x * dv) / figure_u_x;
            return {pivot.x + 28.f * du, world_y, pivot.z + 15.f * dv};
        }
        const float scale = PixelsPerMetre(width, height, zoom);
        const float right = dx / scale;
        const float forward = (dy / scale + (world_y - pivot.y) * std::sin(tilt))
                            / std::cos(tilt);
        const float co = std::cos(yaw), si = std::sin(yaw);
        return {pivot.x + co * right + si * forward, world_y,
                pivot.z - si * right + co * forward};
    }

    // delta is measured from mouse-down; invoke on a copy of the press pose.
    void Pan(float width, float height, float dx, float dy) {
        if (width <= 0.f || height <= 0.f) return;
        anchor.x += dx / width;
        anchor.y += dy / height;
    }

    void Orbit(float dx, float dy) {
        yaw = std::remainder(yaw - dx * .0075f, 6.2831853f);
        tilt = std::clamp(tilt - dy * .009f, -1.32f, 1.32f);
    }

    bool ZoomAt(float width, float height, bool flat, NavPoint pixel, float wheel,
                NavWorld world_hit) {
        if (width <= 0.f || height <= 0.f || !std::isfinite(wheel)) return false;
        const float next = std::clamp(zoom * std::pow(1.14f, std::clamp(wheel, -64.f, 64.f)),
                                      .65f, 4.f);
        if (next == zoom) return false;
        // XY in the flat figure describes the arena ground and carries no height.
        if (flat) world_hit.y = 0.f;
        pivot = world_hit;
        anchor = {pixel.x / width, pixel.y / height};
        zoom = next;
        return true;
    }
};

} // namespace RoboGo::arena
