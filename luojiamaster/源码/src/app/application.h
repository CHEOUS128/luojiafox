#pragma once

#include <pch.h>

#include "app/application_types.h"
#include "platform/platform_backend.h"

namespace RoboGo::app {

LaunchOptions ParseLaunchOptions(const std::vector<std::string> &args);

int RunApplication(platform::PlatformBackend &platform_backend, const LaunchOptions &options);

}// namespace RoboGo::app
