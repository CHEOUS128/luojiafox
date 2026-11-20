#pragma once

#include <pch.h>

namespace RoboGo::app {

enum class Competition { Uc2026, Ul2027, Uc2027 };
enum class CommanderComparison { Arenas, Markers, Layouts };

inline constexpr const char *CompetitionName(Competition competition) {
    switch (competition) {
        case Competition::Uc2026: return "2026 超级对抗赛";
        case Competition::Ul2027: return "2027 高校联盟赛";
        case Competition::Uc2027: return "2027 超级对抗赛";
    }
    return "";
}
inline constexpr int kReturnToLauncher = 220;

struct VideoFrame {
    std::vector<uint8_t> buffer;
    int width = 0;
    int height = 0;
    // Official video can own one slot from the bounded aligned conversion pool.
    // CBB and teammate frames retain their existing vector ownership and layout.
    std::shared_ptr<std::vector<std::uint8_t>> shared_buffer;
    std::size_t pixel_offset = 0;
    std::size_t row_pitch = 0;
    [[nodiscard]] const std::uint8_t *Pixels() const {
        const auto &bytes = shared_buffer ? *shared_buffer : buffer;
        return pixel_offset < bytes.size() ? bytes.data() + pixel_offset : nullptr;
    }
    [[nodiscard]] std::size_t ByteSize() const {
        const auto &bytes = shared_buffer ? *shared_buffer : buffer;
        return pixel_offset <= bytes.size() ? bytes.size() - pixel_offset : 0;
    }
    [[nodiscard]] std::size_t RowPitch() const {
        return row_pitch ? row_pitch : (width > 0 ? static_cast<std::size_t>(width) * 4 : 0);
    }
};

struct UiLogMessage {
    int type = 0;
    std::string text;
};

enum class TextureSlot {
    UdpVideo,
    CustomVideo,
    MiniMap,
    TeammateVideo,
    UlMap,  // Read-only UL venue map. Never replaces the legacy minimap.
    TeammateVideo2,
    TeammateVideo3,
    UcCommanderFlat,
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
    Competition competition = Competition::Ul2027;
    bool local_trial = false;
    bool uc_classic = false;
    bool commander_benchmark = false;
    CommanderComparison commander_comparison = CommanderComparison::Arenas;
    bool benchmark_match_monitor = false;
    std::string commander_video_dir;
    std::string mqtt_server;
    std::string video_port;
    std::string mqtt_host;
    std::string mqtt_port;
    int parsed_video_port = 0;
    bool invert_udp_video = false;

    // Optional manual single-link diagnostics. An ordinary login selects peers by seat.
    std::string teammate_peer;
    std::string teammate_listen;
    bool teammate_ini_unreadable = false;
    bool teammate_off = false;
};

}// namespace RoboGo::app
