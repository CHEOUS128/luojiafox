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
    // 队友第一视角（2027 互联功能），没有 --teammate-* 参数时永远不会被创建
    TeammateVideo,
};

using VideoFrameHandler = std::function<void(VideoFrame &&frame)>;

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

    // 队友画面（两台自定义客户端用网线直连）。
    // 默认就启用（2026-11-02 CH 定：双击 exe 就该有，不能让人再放文件）。
    // 默认 A 机：找 10.77.0.2；B 机在标题条上点一下角色标签，改成找 10.77.0.1。
    // 优先级：命令行 --teammate-*  >  exe 同目录的 队友视角.ini  >  内置默认 A 机。
    std::string teammate_peer;// host:port，发给他
    std::string teammate_listen;// port，收他的
    bool teammate_enabled = false;
    // 用户自己在窗口里选的角色：'A' / 'B'，0 = 还没手动选过。
    // 存在 exe 同目录的 ini 里，下次打开还是这个角色。
    char teammate_role = 0;
    // 配置文件存在但打不开（占用/权限/编码），界面上单独提示
    bool teammate_ini_unreadable = false;
    // 真：参数来自 队友视角.ini（不是命令行）
    bool teammate_from_ini = false;
    // 真：参数来自命令行 --teammate-*
    bool teammate_from_command_line = false;
    // 真：参数来自内置默认（既没命令行也没 ini）——标题条上会标 [默认A]
    bool teammate_from_default = false;
    // 真：队友视角.ini 里写了 peer=off，这次彻底不启用（不出小窗、不占端口）
    bool teammate_off = false;
};

}// namespace RoboGo::app
