#include "app/commander_measurement.h"
#include <iostream>

int main() {
    using namespace std::chrono_literals;
    using M = RoboGo::app::CommanderMeasurement;
    const M::Time t{};
    M m;
    m.Start(t, 100ms);
    m.Present(t + 10ms, 7);
    m.Present(t + 30ms, 1);
    m.Present(t + 80ms, 3);
    m.Present(t + 100ms, 7); // Next window.
    auto r = m.Finish();
    if (r.presents != 3 || r.lanes[0].frames != 3 || r.lanes[1].frames != 2 ||
        r.lanes[2].frames != 1 || r.lanes[2].longest_gap_ms != 90 ||
        r.lanes[1].longest_gap_ms != 70 || r.lanes[0].longest_gap_ms != 50 ||
        r.p95_interval_ms != 50 || r.intervals_over_33ms != 1) return 1;
    m.Start(t, 1s);
    r = m.Finish();
    if (r.lanes[0].frames || r.lanes[0].longest_gap_ms != 1000 || r.presents) return 2;
    std::cout << "PASS: newly visible frames, edge freezes, percentile and window reset\n";
}
