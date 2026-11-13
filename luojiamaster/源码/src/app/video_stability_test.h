#pragma once

#include <string>
#include <vector>

namespace RoboGo::platform { class PlatformBackend; }
namespace RoboGo::app {

// Explicit offline branch, before any BackgroundServices/network/input objects.
int RunVideoStabilityTest(platform::PlatformBackend &backend, const std::vector<std::string> &args);

} // namespace RoboGo::app
