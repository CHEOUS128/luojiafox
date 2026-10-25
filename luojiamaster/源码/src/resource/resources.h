#pragma once

#include <pch.h>

namespace RoboGo::resource {

struct DecodedImage {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;

    // 小地图传完纹理就不用再留着了：1285x711x4 ≈ 3.7 MB，视频帧排队时这段内存很值钱。
    void release() {
        pixels.clear();
        pixels.shrink_to_fit();
    }
};

std::optional<DecodedImage> LoadEmbeddedImage(std::span<const uint8_t> encoded_bytes,
                                              std::string_view resource_name = {});

std::optional<DecodedImage> LoadEmbeddedMapImage();

}// namespace RoboGo::resource
