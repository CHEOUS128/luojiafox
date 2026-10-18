#include "rgba_image_transform.h"

namespace RoboGo::video {

bool ApplyRgba180Rotation(std::vector<uint8_t> &rgba,
                          const int width,
                          const int height,
                          const bool enabled) {
    if (width < 0 || height < 0) {
        return false;
    }

    const auto width_size = static_cast<std::size_t>(width);
    const auto height_size = static_cast<std::size_t>(height);
    constexpr std::size_t channels = 4;
    if (height_size != 0 && width_size > std::numeric_limits<std::size_t>::max() / height_size) {
        return false;
    }
    const std::size_t pixel_count = width_size * height_size;
    if (pixel_count > std::numeric_limits<std::size_t>::max() / channels
        || rgba.size() != pixel_count * channels) {
        return false;
    }
    if (!enabled || pixel_count < 2) {
        return true;
    }

    for (std::size_t left = 0, right = pixel_count - 1; left < right; ++left, --right) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            std::swap(rgba[left * channels + channel], rgba[right * channels + channel]);
        }
    }
    return true;
}

}// namespace RoboGo::video
