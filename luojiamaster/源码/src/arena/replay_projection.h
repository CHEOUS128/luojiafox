#pragma once
#include "arena/nav_camera.h"

#include <algorithm>
#include <cmath>

namespace RoboGo::arena {

struct ReplayPoint { float x, y; };

// Three map landmarks tie the local sample plane to the *image*, excluding
// its border. Coordinates are pixels in the embedded 903 x 499 rule figure.
// The lower-right R pad is a separate check: (843.0,395.4) vs (844,395).
inline ReplayPoint RuleFigurePoint(float u, float v) {
    const auto p=NavCamera::FigurePoint({(u-.5f)*28.f,0,(v-.5f)*15.f});
    return {p.x,p.y};
}

// The mesh shader and the screen-space labels use the same orthographic
// transform. Changing raster resolution for caching does not move a label.
struct ReplayProjection {
    float co, si, ct, st, sx, sy;

    static ReplayProjection ForCanvas(float width, float height, float yaw, float tilt, float zoom) {
        const float co = std::cos(yaw), si = std::sin(yaw);
        const float ct = std::cos(tilt), st = std::sin(tilt);
        const float px=NavCamera::PixelsPerMetre(width,height,zoom);
        return {co, si, ct, st, 2.f * px / width, 2.f * px / height};
    }

    ReplayPoint SamplePlaneUV(float u, float v, float height = 0) const {
        const float x = (u - .5f) * 28.f;
        const float z = (v - .5f) * 15.f;
        const float right = x * co - z * si;
        const float down = (x * si + z * co) * ct - height * st;
        return {.5f + .5f * sx * right, .5f + .5f * sy * down};
    }
};

} // namespace RoboGo::arena
