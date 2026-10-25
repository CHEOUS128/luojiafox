#pragma once

#include <pch.h>

namespace RoboGo::resource {

struct DecodedImage {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
};

std::optional<DecodedImage> LoadEmbeddedImage(std::span<const uint8_t> encoded_bytes,
                                              std::string_view resource_name = {});

std::optional<DecodedImage> LoadEmbeddedMapImage();

}// namespace RoboGo::resource
