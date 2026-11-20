#include "arena/nav_camera.h"

#include <array>
#include <cstdio>
#include <cstdlib>

using RoboGo::arena::NavCamera;
using RoboGo::arena::NavPoint;
using RoboGo::arena::NavWorld;

namespace {
void check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
bool near(float a, float b) { return std::abs(a - b) < .002f; }
bool same(NavPoint a, NavPoint b) { return near(a.x, b.x) && near(a.y, b.y); }
bool same(NavWorld a, NavWorld b) {
    return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}
}

int main() {
    constexpr float width = 1200.f, height = 900.f;
    NavCamera camera;
    const float scale = camera.PixelsPerMetre(width, height, camera.zoom);
    const auto near_edge = camera.Project(width, height, false, {0.f, 0.f, 5.f});
    camera.Orbit(40.f, 30.f);
    check(camera.Project(width, height, false, {0.f, 0.f, 5.f}).x > near_edge.x,
          "dragging right rotates the near edge right");
    check(camera.Project(width, height, false, {0.f, 1.f, 0.f}).y > height * .5f,
          "dragging down tilts an elevated point down");

    for (float yaw : {0.f, .79f, 1.57f, -2.f, 3.13f}) {
        camera.yaw = yaw;
        for (float tilt : {-1.32f, -.68f, 0.f, .68f, 1.32f}) {
            camera.tilt = tilt;
            const auto a = camera.Project(width, height, false, {});
            const auto b = camera.Project(width, height, false,
                                          {std::cos(yaw), 0.f, -std::sin(yaw)});
            check(near(b.x - a.x, scale) && near(a.y, b.y),
                  "orbit preserves pixels per horizontal metre");
        }
    }
    for (bool flat : {true, false}) {
        camera = {};
        camera.Orbit(-53.f, 72.f);
        const std::array<NavWorld,3> landmarks{{
            {-9.f, 0.f, -5.f}, {5.2f, 2.5f, 3.6f}, {8.f, .8f, -1.f}
        }};
        for (auto landmark : landmarks) {
            if (flat) landmark.y = 0.f;
            const auto pixel = camera.Project(width, height, flat, landmark);
            check(same(camera.PickAtHeight(width, height, flat, pixel, landmark.y), landmark),
                  "recover the same world point at a known height");
            const auto other_before = camera.Project(width, height, flat, {});
            const float original_zoom = camera.zoom;
            for (float wheel : {1.f, 2.f, -3.f}) {
                camera.ZoomAt(width, height, flat, pixel, wheel, landmark);
                check(same(camera.Project(width, height, flat, landmark), pixel),
                      "wheel pins both ground and elevated landmarks to the cursor");
            }
            check(near(camera.zoom, original_zoom) &&
                  same(camera.Project(width, height, flat, {}), other_before),
                  "reciprocal wheel steps restore the full view");

            camera.Orbit(42.f, -55.f);
            check(same(camera.Project(width, height, flat, landmark), pixel),
                  "rotation about the selected point keeps it fixed");
            camera.Pan(width, height, 105.f, -61.f);
            const NavPoint moved{pixel.x + 105.f, pixel.y - 61.f};
            check(same(camera.Project(width, height, flat, landmark), moved),
                  "pan tracks the pointer in both axes");
            check(same(camera.Project(width, height, !flat, landmark), moved),
                  "switching display retains the focus point");
            const NavPoint resized{moved.x * 1.6f, moved.y * 1.2f};
            check(same(camera.Project(1920.f, 1080.f, flat, landmark), resized),
                  "resizing keeps the normalised pivot screen location");
        }
    }

    const auto off = camera.OffsetNdc(width, height);
    const auto ground = camera.Project(width, height, false, {});
    check(near((off.x + 1.f) * width / 2.f, ground.x) &&
          near((1.f - off.y) * height / 2.f, ground.y),
          "GPU clip offset agrees with UI label position");
    camera.ZoomAt(width, height, false, {300.f, 200.f}, 100.f, {3.f, 1.f, 4.f});
    const auto held = camera.pivot;
    check(camera.zoom == 4.f &&
          !camera.ZoomAt(width, height, false, {800.f, 400.f}, 1.f, {}) &&
          same(camera.pivot, held), "wheel at maximum does not move the pivot");
    camera.ZoomAt(width, height, false, {800.f, 400.f}, -100.f, {});
    check(camera.zoom == .65f, "wheel has a lower bound");
    camera.Orbit(100000.f, -100000.f);
    check(std::abs(camera.yaw) <= 3.142f && near(camera.tilt, 1.32f),
          "large motion wraps yaw and clamps tilt");
    camera = {};
    check(same(camera.Project(width, height, false, {}), {width/2, height/2}) &&
          camera.yaw == 0.f && camera.tilt == 0.f && camera.zoom == 1.f,
          "reset returns to full-field top view");
    std::puts("PASS: orbit scale, cursor anchoring, pan, pivot, mode/size changes and bounds");
    return 0;
}
