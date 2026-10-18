#pragma once

#include <pch.h>

namespace RoboGo::video {

bool ApplyRgba180Rotation(std::vector<uint8_t> &rgba,
                          int width,
                          int height,
                          bool enabled);

}// namespace RoboGo::video
