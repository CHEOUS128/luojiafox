#pragma once

#include <pch.h>

namespace RoboGo::app {

struct VideoFrame {
    std::vector<uint8_t> buffer;
    int width = 0;
    int height = 0;
};

struct UiLogMessage {
    int type = 0;
    std::string text;
};

enum class TextureSlot {
    UdpVideo,
    CustomVideo,
    MiniMap,
};

struct TextureView {
    ImTextureRef texture_ref;
    ImVec2 size = ImVec2(0.0f, 0.0f);

    [[nodiscard]] bool IsReady() const {
        return texture_ref.GetTexID() != ImTextureID_Invalid
               && size.x > 0.0f
               && size.y > 0.0f;
    }
};

struct LaunchOptions {
    std::string mqtt_server;
    std::string video_port;
    std::string mqtt_host;
    std::string mqtt_port;
    int parsed_video_port = 0;
    bool invert_udp_video = false;
};

}// namespace RoboGo::app
