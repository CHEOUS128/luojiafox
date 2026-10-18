#define STB_IMAGE_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <pch.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include "resource/resources.h"
#include "embedded_map_png.h"

namespace RoboGo::resource {

std::optional<DecodedImage> LoadEmbeddedImage(const std::span<const uint8_t> encoded_bytes,
                                              const std::string_view resource_name) {
    const std::string_view display_name = resource_name.empty() ? "内嵌资源" : resource_name;
    if (encoded_bytes.empty()) {
        SPDLOG_ERROR("{}解码失败 reason=empty resource bytes", display_name);
        return std::nullopt;
    }

    int width = 0;
    int height = 0;
    int channels_in_file = 0;
    stbi_uc *decoded_pixels = stbi_load_from_memory(
            encoded_bytes.data(),
            static_cast<int>(encoded_bytes.size()),
            &width,
            &height,
            &channels_in_file,
            STBI_rgb_alpha);
    if (decoded_pixels == nullptr) {
        const char *failure_reason = stbi_failure_reason();
        SPDLOG_ERROR("{}解码失败 reason={}", display_name, failure_reason != nullptr ? failure_reason : "unknown");
        return std::nullopt;
    }

    DecodedImage image;
    image.width = width;
    image.height = height;
    const std::size_t pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    image.pixels.assign(decoded_pixels, decoded_pixels + pixel_count);
    stbi_image_free(decoded_pixels);

    SPDLOG_INFO("{}已解码 width={} height={} channels={}", display_name, width, height, channels_in_file);
    return image;
}

std::optional<DecodedImage> LoadEmbeddedMapImage() {
    return LoadEmbeddedImage(
            std::span<const uint8_t>(embedded_map_png_data, static_cast<std::size_t>(embedded_map_png_data_len)),
            "内嵌小地图");
}

}// namespace RoboGo::resource
