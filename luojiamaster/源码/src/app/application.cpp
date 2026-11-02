#include "application.h"

#include <cctype>

#include "keyboard_mouse_state_manager.h"
#include "main_window.h"
#include "net/teammate_link.h"
#include "video/custom_byte_block_video_decoder_worker.h"
#include "video/rgba_image_transform.h"
#include "video/teammate_video_receiver.h"
#include "video/video_stream_manager.h"

namespace {

using RoboGo::app::LaunchOptions;
using RoboGo::app::TextureSlot;
using RoboGo::app::TextureView;
using RoboGo::app::UiLogMessage;
using RoboGo::app::VideoFrame;

constexpr bool kDefaultInvertUdpVideo = true;

std::string DefaultMqttServer() {
    // return "tcp://127.0.0.1:3333";// 模拟裁判系统
    return "tcp://192.168.12.1:3333";// 正式比赛
    // return "tcp://192.168.1.2:3333";// 自己部署的裁判系统
}

// 队友视角的一键启动配置：默认就启用（双击 exe 即可），不需要任何文件或参数。
//
// 优先级（高 → 低）：
//   1. 命令行 --teammate-peer / --teammate-listen（调试、本机双实例用）
//   2. exe 同目录的 队友视角.ini（现场改 IP / 固定角色用）
//   3. 内置默认：A 机 —— peer=10.77.0.2:47700，listen=47700
//
// 为什么默认是 A 机：两台客户端里总有一台要先能用；默认是 A（步兵那一侧），
// 另一台在标题条上点「角色」切成 B（10.77.0.2 → 10.77.0.1），选择会写回 ini。
//
// 文件格式（UTF-8，一行一项，'#' 开头是注释）：
//   peer=10.77.0.2:47700
//   listen=47700
//   role=B                # 可选：A / B
constexpr const char *kTeammateConfigFileName = "队友视角.ini";
// 内置默认角色（A 机）
constexpr const char *kTeammateDefaultPeer = "10.77.0.2:47700";
constexpr const char *kTeammateDefaultListen = "47700";
// B 机 = 把 A 的这两项对调
constexpr const char *kTeammateRoleBPeer = "10.77.0.1:47700";
#ifdef _WIN32
// 拼路径必须用宽字符：中文 Windows 的窄字符 API 走 GBK，
// 直接传 UTF-8 的 "队友视角.ini" 会找不到文件。
constexpr const wchar_t *kTeammateConfigFileNameW = L"队友视角.ini";
#endif

struct TeammateConfig {
    bool found = false;
    bool unreadable = false;
    std::string peer;
    std::string listen;
    char role = 0;  // 'A' / 'B' / 0 = 没写
};

[[nodiscard]] TeammateConfig LoadTeammateConfig() {
    TeammateConfig config;
    std::error_code ec;
    std::filesystem::path exe_dir = std::filesystem::current_path(ec);
    if (ec) {
        exe_dir.clear();
    }

#ifdef _WIN32
    // 以 exe 所在目录为准：双击、快捷方式、从别的目录起都指向同一个文件。
    wchar_t module_path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        exe_dir = std::filesystem::path(module_path).parent_path();
    }
#endif

    if (exe_dir.empty()) {
        return config;
    }

    // Windows 下整条路径都必须是宽字符；其它平台用 UTF-8 的窄路径。
#ifdef _WIN32
    const std::filesystem::path ini_path = exe_dir / kTeammateConfigFileNameW;
#else
    const std::filesystem::path ini_path = exe_dir / kTeammateConfigFileName;
#endif
    std::error_code exists_ec;
    if (!std::filesystem::exists(ini_path, exists_ec) || exists_ec) {
        return config;
    }

    std::ifstream stream(ini_path);
    if (!stream.is_open()) {
        config.unreadable = true;
        return config;
    }

    config.found = true;
    std::string line;
    while (std::getline(stream, line)) {
        // 记事本另存为「UTF-8 带 BOM」时首行会多 3 个字节，去掉，否则 peer= 匹配不上。
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF
            && static_cast<unsigned char>(line[1]) == 0xBB
            && static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }
        const size_t equals = line.find('=', first);
        if (equals == std::string::npos) {
            continue;
        }
        std::string key = line.substr(first, equals - first);
        std::string value = line.substr(equals + 1);
        const size_t key_last = key.find_last_not_of(" \t");
        key = (key_last == std::string::npos) ? std::string() : key.substr(0, key_last + 1);
        const size_t value_first = value.find_first_not_of(" \t");
        value = (value_first == std::string::npos) ? std::string() : value.substr(value_first);
        const size_t value_last = value.find_last_not_of(" \t");
        if (value_last != std::string::npos) {
            value = value.substr(0, value_last + 1);
        }

        if (key == "peer") {
            config.peer = value;
        } else if (key == "listen") {
            config.listen = value;
        } else if (key == "role" || key == "角色") {
            // 只认 A / B（大小写都行），其它忽略
            if (!value.empty()) {
                const char c = (value[0] == 'a' || value[0] == 'A')   ? 'A'
                               : (value[0] == 'b' || value[0] == 'B') ? 'B'
                                                                      : 0;
                config.role = c;
            }
        }
    }
    return config;
}

// 记住用户在窗口里选的角色：写 exe 同目录的 队友视角.ini（保留原来的注释和别的键）。
// 只改 role= 这一行；没有就追加到末尾。写失败不算错误——本次运行照样是选中的那个角色。
void SaveTeammateRole(const char role) {
    if (role != 'A' && role != 'B') {
        return;
    }
    std::error_code ec;
    std::filesystem::path exe_dir = std::filesystem::current_path(ec);
#ifdef _WIN32
    wchar_t module_path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        exe_dir = std::filesystem::path(module_path).parent_path();
    }
#endif
    if (exe_dir.empty()) {
        return;
    }
#ifdef _WIN32
    const std::filesystem::path ini_path = exe_dir / kTeammateConfigFileNameW;
#else
    const std::filesystem::path ini_path = exe_dir / kTeammateConfigFileName;
#endif

    std::vector<std::string> lines;
    bool replaced = false;
    {
        std::ifstream input(ini_path);
        std::string line;
        while (std::getline(input, line)) {
            std::string probe = line;
            if (!probe.empty() && probe.back() == '\r') {
                probe.pop_back();
            }
            // 允许 role= 前面有空格
            const size_t first = probe.find_first_not_of(" \t");
            const std::string body = (first == std::string::npos) ? std::string() : probe.substr(first);
            std::string lowered = body;
            for (char &c : lowered) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            const bool is_role = lowered.starts_with("role=") || body.starts_with("角色=");
            if (is_role) {
                lines.push_back(std::string("role=") + role);
                replaced = true;
            } else {
                lines.push_back(probe);
            }
        }
    }
    if (!replaced) {
        lines.push_back(std::string("role=") + role);
    }

    std::ofstream output(ini_path, std::ios::trunc);
    if (!output.is_open()) {
        return;
    }
    // 写 CRLF：记事本（含老版本）打开才不会连成一行。解析端会自己去掉 '\r'。
    for (const std::string &line : lines) {
        output << line << "\r\n";
    }
}

void ParseNetworkEndpoints(LaunchOptions &options) {
    options.mqtt_host = "127.0.0.1";
    options.mqtt_port = "3333";
    if (options.mqtt_server.starts_with("tcp://")) {
        std::string address = options.mqtt_server.substr(6);
        const size_t colon_pos = address.find(':');
        if (colon_pos != std::string::npos) {
            options.mqtt_host = address.substr(0, colon_pos);
            options.mqtt_port = address.substr(colon_pos + 1);
        }
    }
    options.parsed_video_port = std::stoi(options.video_port);
}

class AvFrameRgbaConverter {
public:
    explicit AvFrameRgbaConverter(const bool invert_udp_video)
        : invert_udp_video_(invert_udp_video) {
    }

    ~AvFrameRgbaConverter() {
        if (sws_context_ != nullptr) {
            sws_freeContext(sws_context_);
            sws_context_ = nullptr;
        }
    }

    std::optional<VideoFrame> convert(const AVFrame *frame) {
        if (frame == nullptr) {
            return std::nullopt;
        }

        if (frame->format != AV_PIX_FMT_NV12 && frame->format != AV_PIX_FMT_YUV420P) {
            return std::nullopt;
        }

        sws_context_ = sws_getCachedContext(
                sws_context_,
                frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                frame->width, frame->height, AV_PIX_FMT_RGBA,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (sws_context_ == nullptr) {
            return std::nullopt;
        }

        VideoFrame video_frame;
        video_frame.buffer.resize(static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height) * 4u);
        video_frame.width = frame->width;
        video_frame.height = frame->height;
        uint8_t *dest_data[1] = {video_frame.buffer.data()};
        int dest_linesize[1] = {frame->width * 4};
        const int scaled_height = sws_scale(sws_context_,
                                            frame->data, frame->linesize,
                                            0, frame->height,
                                            dest_data, dest_linesize);
        if (scaled_height != frame->height) {
            return std::nullopt;
        }
        // 180° 旋转已改到渲染阶段用 GPU 的 UV 翻转完成（见 main_window.cpp），
        // 不再在这里用 CPU 逐像素交换整帧 1080p 数据——那是操作延迟的主要来源之一。
        return video_frame;
    }

    void SetInvertUdpVideo(const bool enabled) {
        invert_udp_video_.store(enabled, std::memory_order_relaxed);
    }

    [[nodiscard]] bool IsInvertUdpVideoEnabled() const {
        return invert_udp_video_.load(std::memory_order_relaxed);
    }

private:
    SwsContext *sws_context_ = nullptr;
    std::atomic<bool> invert_udp_video_ = false;
};

class BackgroundServices {
public:
    explicit BackgroundServices(const LaunchOptions &options)
        : udp_frame_converter_(options.invert_udp_video),
          custom_byte_block_decoder_worker_(
                  [this](RoboGo::video::CustomByteBlockDecodedFrame &&decoded_frame) {
                      VideoFrame custom_video_frame;
                      custom_video_frame.buffer = std::move(decoded_frame.rgba);
                      custom_video_frame.width = decoded_frame.width;
                      custom_video_frame.height = decoded_frame.height;
                      custom_video_frame_queue_.emplace(std::move(custom_video_frame));
                  },
                  [this](const int type, const std::string &text) {
                      UiLogMessage log_message;
                      log_message.type = type;
                      log_message.text = text;
                      custom_video_log_queue_.emplace(std::move(log_message));
                  }),
          mqtt_client_(io_context_, options.mqtt_host, options.mqtt_port, [this](const std::string &topic, const std::string &payload) {
              message_queue_.emplace(topic, payload);
          }),
          video_stream_manager_(io_context_, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), options.parsed_video_port), [this](const AVFrame *frame) {
              const auto video_frame = udp_frame_converter_.convert(frame);
              if (video_frame.has_value()) {
                  video_frame_queue_.emplace(video_frame.value());
              } else if (frame == nullptr) {
                  SPDLOG_ERROR("收到空视频帧指针，已丢弃");
              } else {
                  SPDLOG_ERROR("UDP 视频帧转换或方向处理失败 format={} width={} height={}",
                               frame->format,
                               frame->width,
                               frame->height);
              }
          }, [this](const std::vector<uint8_t> &frame) {
              // 队友画面旁路：官方 UDP 图传的原始 HEVC 字节，原样转发给队友。
              // 没启用队友视角（既无 --teammate-* 也无 队友视角.ini）时 teammate_link_ 为空，这里什么都不做。
              if (teammate_link_ != nullptr) {
                  teammate_link_->push_frame(frame);
              }
          }),
          io_thread_([this]() {
              SPDLOG_INFO("IO线程已启动");
              io_context_.run();
              SPDLOG_INFO("IO线程已退出");
          }) {
        // 队友链路放在最后初始化：没启用（既无 --teammate-* 也无 队友视角.ini）就完全不建，原有逻辑一行都不变。
        // 收到对方的码流 → 交给解码器 → 解出来的 RGBA 进队列，由主循环上传纹理。
        if (options.teammate_enabled) {
            RoboGo::net::TeammateLinkConfig link_config;
            if (!options.teammate_peer.empty()) {
                std::string endpoint_error;
                if (!RoboGo::net::TeammateLink::ParseEndpoint(options.teammate_peer,
                                                              link_config.peer,
                                                              endpoint_error)) {
                    SPDLOG_ERROR("队友视角 peer 解析失败（--teammate-peer 或 队友视角.ini）value={} error={}",
                                 options.teammate_peer,
                                 endpoint_error);
                }
            }
            if (!options.teammate_listen.empty()) {
                try {
                    const unsigned long port = std::stoul(options.teammate_listen);
                    if (port > 0 && port <= 65535) {
                        link_config.listen_port = static_cast<std::uint16_t>(port);
                    } else {
                        SPDLOG_ERROR("队友视角 listen 端口越界（--teammate-listen 或 队友视角.ini）value={}", options.teammate_listen);
                    }
                } catch (const std::exception &) {
                    SPDLOG_ERROR("队友视角 listen 不是合法端口（--teammate-listen 或 队友视角.ini）value={}", options.teammate_listen);
                }
            }

            if (!link_config.peer.host.empty() || link_config.listen_port != 0) {
                teammate_video_receiver_ = std::make_unique<RoboGo::video::TeammateVideoReceiver>(
                        RoboGo::video::TeammateVideoConfig{},
                        [this](VideoFrame &&video_frame) {
                            teammate_video_frame_queue_.emplace(std::move(video_frame));
                        });
                teammate_link_ = std::make_unique<RoboGo::net::TeammateLink>(
                        link_config,
                        [this](std::vector<std::uint8_t> &&frame) {
                            teammate_video_receiver_->submit_frame(std::move(frame));
                        });
                teammate_link_->set_local_inverted(udp_frame_converter_.IsInvertUdpVideoEnabled());
                SPDLOG_INFO("队友画面链路已启用 peer={}:{} listen_port={}",
                            link_config.peer.host,
                            link_config.peer.port,
                            link_config.listen_port);
            } else {
                SPDLOG_WARN("队友画面链路未启用：peer / listen 都没解析出有效值（--teammate-* 或 队友视角.ini）");
            }
        }
    }

    ~BackgroundServices() {
        Stop();
    }

    BackgroundServices(const BackgroundServices &) = delete;
    BackgroundServices &operator=(const BackgroundServices &) = delete;

    RoboGo::io::MqttClient &mqtt_client() {
        return mqtt_client_;
    }

    void SetUdpVideoInverted(const bool enabled) {
        udp_frame_converter_.SetInvertUdpVideo(enabled);
        // 队友那一路是原样透传的字节，翻转状态得跟着一起同步，
        // 否则队友画面的朝向会和我们主画面不一致。
        if (teammate_link_ != nullptr) {
            teammate_link_->set_local_inverted(enabled);
        }
    }

    [[nodiscard]] bool IsUdpVideoInverted() const {
        return udp_frame_converter_.IsInvertUdpVideoEnabled();
    }

    [[nodiscard]] bool TeammateLinkEnabled() const {
        return teammate_link_ != nullptr;
    }

    // 标题条切角色：只换发送目标。监听端口不动（两端都是 47700）。
    void SetTeammatePeer(const RoboGo::net::TeammateEndpoint &endpoint) {
        if (teammate_link_ != nullptr) {
            teammate_link_->set_peer(endpoint);
        }
    }

    [[nodiscard]] RoboGo::net::TeammateLinkStats TeammateStats() const {
        return teammate_link_ != nullptr ? teammate_link_->stats() : RoboGo::net::TeammateLinkStats{};
    }

    [[nodiscard]] RoboGo::video::TeammateVideoStats TeammateVideoStatsSnapshot() const {
        return teammate_video_receiver_ != nullptr ? teammate_video_receiver_->stats()
                                                   : RoboGo::video::TeammateVideoStats{};
    }

    // 只在按 L 接管主画面后用：在途帧全丢掉，让主画面显示的是真正的最新一帧，
    // 而不是排队积压了几百毫秒的旧画面。按回来的 L 也走这里，保证状态一致。
    void DropTeammateVideoFrames() {
        TakeLatestTeammateVideoFrame();
    }

    void ProcessMessages(State &state) {
        std::pair<std::string, std::string> message;
        while (message_queue_.try_dequeue(message)) {
            HandleMqttMessage(state, mqtt_client_, message.first, message.second, [this](std::span<const uint8_t> packet_bytes) {
                custom_byte_block_decoder_worker_.enqueue_packet_bytes(packet_bytes);
            });
        }
    }

    void DrainCustomVideoLogs(State &state) {
        UiLogMessage log_message;
        while (custom_video_log_queue_.try_dequeue(log_message)) {
            AppendVehicleLog(state, log_message.type, log_message.text);
        }
    }

    std::optional<VideoFrame> TakeLatestVideoFrame() {
        return TakeLatestFrame(video_frame_queue_);
    }

    std::optional<VideoFrame> TakeLatestCustomVideoFrame() {
        return TakeLatestFrame(custom_video_frame_queue_);
    }

    std::optional<VideoFrame> TakeLatestTeammateVideoFrame() {
        return TakeLatestFrame(teammate_video_frame_queue_);
    }

    // 给主循环的观测探针用：只读计数，不改任何状态。
    const RoboGo::video::CustomByteBlockVideoDecoderWorker &custom_video_decoder() const {
        return custom_byte_block_decoder_worker_;
    }

    void Stop() {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        mqtt_client_.stop();
        // 先停收发线程，再停解码线程——接收线程会往解码队列里投帧。
        if (teammate_link_ != nullptr) {
            teammate_link_->stop();
        }
        video_stream_manager_.stop();
        io_context_.stop();
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
        if (teammate_video_receiver_ != nullptr) {
            teammate_video_receiver_->stop();
        }
        custom_byte_block_decoder_worker_.stop();
    }

private:
    static std::optional<VideoFrame> TakeLatestFrame(moodycamel::ReaderWriterQueue<VideoFrame> &queue) {
        std::optional<VideoFrame> latest_frame;
        VideoFrame video_frame;
        while (queue.try_dequeue(video_frame)) {
            latest_frame = std::move(video_frame);
        }
        return latest_frame;
    }

    boost::asio::io_context io_context_;
    moodycamel::ReaderWriterQueue<std::pair<std::string, std::string>> message_queue_;
    moodycamel::ReaderWriterQueue<VideoFrame> video_frame_queue_;
    moodycamel::ReaderWriterQueue<VideoFrame> custom_video_frame_queue_;
    moodycamel::ReaderWriterQueue<VideoFrame> teammate_video_frame_queue_;
    moodycamel::ReaderWriterQueue<UiLogMessage> custom_video_log_queue_;
    AvFrameRgbaConverter udp_frame_converter_;
    RoboGo::video::CustomByteBlockVideoDecoderWorker custom_byte_block_decoder_worker_;
    RoboGo::io::MqttClient mqtt_client_;
    RoboGo::video::VideoStreamManager video_stream_manager_;
    // 声明顺序有讲究：teammate_link_ 的接收线程会回调 teammate_video_receiver_，
    // 所以 receiver 必须先构造（先于 link 声明），析构时反而后销毁。
    std::unique_ptr<RoboGo::video::TeammateVideoReceiver> teammate_video_receiver_;
    std::unique_ptr<RoboGo::net::TeammateLink> teammate_link_;
    std::thread io_thread_;
    bool stopped_ = false;
};

void UploadLatestVideoFrames(RoboGo::platform::PlatformBackend &platform_backend,
                             BackgroundServices &background_services,
                             std::chrono::steady_clock::time_point &last_udp_frame_time,
                             std::chrono::steady_clock::time_point &last_custom_frame_time,
                             std::chrono::steady_clock::time_point &last_teammate_frame_time,
                             std::uint64_t &udp_frame_count,
                             std::uint64_t &custom_frame_count,
                             std::uint64_t &teammate_frame_count) {
    const auto video_frame = background_services.TakeLatestVideoFrame();
    if (video_frame.has_value()) {
        last_udp_frame_time = std::chrono::steady_clock::now();
        ++udp_frame_count;
        if (!platform_backend.UploadTexture(TextureSlot::UdpVideo, *video_frame)) {
            SPDLOG_ERROR("创建UDP视频纹理失败 width={} height={}", video_frame->width, video_frame->height);
        }
    }

    const auto custom_video_frame = background_services.TakeLatestCustomVideoFrame();
    if (custom_video_frame.has_value()) {
        last_custom_frame_time = std::chrono::steady_clock::now();
        ++custom_frame_count;
        if (!platform_backend.UploadTexture(TextureSlot::CustomVideo, *custom_video_frame)) {
            SPDLOG_ERROR("创建 CustomByteBlock 视频纹理失败 width={} height={}",
                         custom_video_frame->width,
                         custom_video_frame->height);
        }
    }

    // 队友画面：没启用链路时队列永远是空的，这里不会付出任何代价。
    const auto teammate_video_frame = background_services.TakeLatestTeammateVideoFrame();
    if (teammate_video_frame.has_value()) {
        last_teammate_frame_time = std::chrono::steady_clock::now();
        ++teammate_frame_count;
        if (!platform_backend.UploadTexture(TextureSlot::TeammateVideo, *teammate_video_frame)) {
            SPDLOG_ERROR("创建队友画面纹理失败 width={} height={}",
                         teammate_video_frame->width,
                         teammate_video_frame->height);
        }
    }
}

[[nodiscard]] bool IsVideoDisplayReady(const TextureView &video_texture,
                                       const std::chrono::steady_clock::time_point last_frame_time,
                                       const std::chrono::steady_clock::duration fallback_timeout,
                                       const std::chrono::steady_clock::time_point now) {
    return video_texture.IsReady()
           && last_frame_time != std::chrono::steady_clock::time_point::min()
           && (now - last_frame_time) <= fallback_timeout;
}

void UpdateVideoLayoutPreference(State &state,
                                 const bool udp_video_display_ready,
                                 const bool custom_video_display_ready,
                                 const bool custom_video_hold_as_main,
                                 bool &prefer_custom_video_as_main,
                                 bool &using_custom_video_as_main) {
    bool handled_manual_video_layout_toggle = false;
    if (ImGui::IsKeyPressed(ImGuiKey_H, false) && !ImGui::GetIO().WantTextInput) {
        prefer_custom_video_as_main = !prefer_custom_video_as_main;
        handled_manual_video_layout_toggle = true;

        if (prefer_custom_video_as_main) {
            if (custom_video_display_ready) {
                AppendSystemLog(state, 0, "主画面偏好已切换到 CustomByteBlock 视频流。");
            } else if (udp_video_display_ready) {
                AppendSystemLog(state, 1, "主画面偏好已切换到 CustomByteBlock 视频流，等待 CustomByteBlock 视频流恢复后自动切换。");
            } else {
                AppendSystemLog(state, 1, "主画面偏好已切换到 CustomByteBlock 视频流，当前两路视频都不可用。");
            }
        } else {
            if (udp_video_display_ready) {
                AppendSystemLog(state, 0, "主画面偏好已切换到 UDP 视频流。");
            } else if (custom_video_display_ready) {
                AppendSystemLog(state, 1, "主画面偏好已切换到 UDP 视频流，UDP 视频流恢复后会自动切回。");
            } else {
                AppendSystemLog(state, 1, "主画面偏好已切换到 UDP 视频流，当前两路视频都不可用。");
            }
        }
    }

    const bool should_use_custom_video_as_main = custom_video_hold_as_main
                                                 && (prefer_custom_video_as_main || !udp_video_display_ready);
    if (!handled_manual_video_layout_toggle && should_use_custom_video_as_main != using_custom_video_as_main) {
        using_custom_video_as_main = should_use_custom_video_as_main;
        if (using_custom_video_as_main) {
            AppendSystemLog(state, 1, "UDP 视频流无数据，主画面已切换到 CustomByteBlock 视频流");
        } else if (udp_video_display_ready) {
            AppendSystemLog(state, 0, "UDP 视频流恢复，主画面已切回 UDP 视频流");
        }
    } else {
        using_custom_video_as_main = should_use_custom_video_as_main;
    }
}

void ToggleUdpVideoInversion(State &state, BackgroundServices &background_services) {
    const bool udp_video_inverted = !background_services.IsUdpVideoInverted();
    background_services.SetUdpVideoInverted(udp_video_inverted);
    AppendSystemLog(state,
                    0,
                    udp_video_inverted
                            ? "UDP 视频方向已切换为 180 度旋转。"
                            : "UDP 视频方向已切换为正常方向。");
}

// 点队友小窗标题条上的 [A]/[B]：换角色 = 换对方 IP，链路热切目标并写回 ini。
// A 机 peer=10.77.0.2:47700 / B 机 peer=10.77.0.1:47700；listen 两端都是 47700。
// 监听端口不重建：accept 由另一个线程长期持有，热改要整条链路停掉才安全，
// 而两个角色收的是同一个端口，重建没有任何收益。
void ApplyTeammateRole(BackgroundServices &background_services,
                       char &teammate_role,
                       bool &teammate_role_from_default,
                       std::string &teammate_peer,
                       std::string &teammate_listen,
                       State &state) {
    const char next_role = (teammate_role == 'B') ? 'A' : 'B';
    teammate_role = next_role;
    teammate_role_from_default = false;
    teammate_peer = (next_role == 'B') ? kTeammateRoleBPeer : kTeammateDefaultPeer;
    teammate_listen = kTeammateDefaultListen;

    RoboGo::net::TeammateEndpoint endpoint;
    std::string parse_error;
    if (!RoboGo::net::TeammateLink::ParseEndpoint(teammate_peer, endpoint, parse_error)) {
        SPDLOG_ERROR("队友角色切换失败：{} 解析失败 error={}", teammate_peer, parse_error);
        AppendSystemLog(state, 2, std::string("队友角色切换失败：") + parse_error);
        return;
    }
    background_services.SetTeammatePeer(endpoint);
    SaveTeammateRole(next_role);
    SPDLOG_INFO("队友角色已切换为 {} 机 peer={} listen={}",
                next_role,
                teammate_peer,
                teammate_listen);
    AppendSystemLog(state,
                    0,
                    std::string("队友角色已切换为 ") + next_role
                            + " 机（找 " + (next_role == 'B' ? "10.77.0.1" : "10.77.0.2")
                            + "），正在重连。");
}

}// namespace

namespace RoboGo::app {

LaunchOptions ParseLaunchOptions(const std::vector<std::string> &args) {
    LaunchOptions options;
    options.mqtt_server = DefaultMqttServer();
    options.video_port = "3334";
    options.invert_udp_video = kDefaultInvertUdpVideo;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--mqtt_server" && i + 1 < args.size()) {
            options.mqtt_server = args[i + 1];
            ++i;
        } else if (args[i] == "--video_port" && i + 1 < args.size()) {
            options.video_port = args[i + 1];
            ++i;
        } else if (args[i] == "--teammate-peer" && i + 1 < args.size()) {
            // 队友画面：发给谁（host:port）。两台机器各填对方的 IP。
            options.teammate_peer = args[i + 1];
            ++i;
        } else if (args[i] == "--teammate-listen" && i + 1 < args.size()) {
            // 队友画面：在哪个端口收（两台机器都填同一个端口，比如 47700）
            options.teammate_listen = args[i + 1];
            ++i;
        }
    }

    options.teammate_enabled = !options.teammate_peer.empty() || !options.teammate_listen.empty();
    // 只有真正从命令行拿到了参数才算「有来源」：否则 ini 打不开时会被误报成「命令行参数生效」。
    const bool teammate_from_command_line = options.teammate_enabled;

    // 优先级：命令行 --teammate-*  >  exe 同目录的 队友视角.ini  >  内置默认 A 机。
    // 注意：这里不能打日志——ParseLaunchOptions 在 logging::initialize 之前调用。
    const TeammateConfig teammate_config = LoadTeammateConfig();
    options.teammate_ini_unreadable = teammate_config.unreadable;
    options.teammate_from_command_line = teammate_from_command_line;

    if (teammate_from_command_line) {
        // 命令行给了就用命令行的；ini 里的 role 只用来显示（本机双实例测试用，不跟角色走）
        options.teammate_role = teammate_config.role;
    } else if (teammate_config.found) {
        options.teammate_role = teammate_config.role;
        // peer=off：现场嫌它碍事时写这一行，整条链路都不建（和以前没这个功能时一模一样）
        std::string lowered_peer = teammate_config.peer;
        for (char &c : lowered_peer) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lowered_peer == "off" || lowered_peer == "0") {
            options.teammate_off = true;
            options.teammate_enabled = false;
        } else if (teammate_config.role != 0 && teammate_config.peer.empty()) {
            // 只写了 role= 就按角色定地址：标题条上切角色写的就是这一行，
            // 所以「切完重启」不会因为 ini 里还留着旧 peer= 而回退。
            options.teammate_peer = teammate_config.role == 'B' ? kTeammateRoleBPeer : kTeammateDefaultPeer;
            options.teammate_listen = teammate_config.listen.empty() ? kTeammateDefaultListen
                                                                     : teammate_config.listen;
            options.teammate_from_ini = true;
        } else if (!teammate_config.peer.empty() || !teammate_config.listen.empty()) {
            // 写了 peer= 就以 peer= 为准（本机双实例 / 非 10.77 网段的老现场配置）
            options.teammate_peer = teammate_config.peer;
            options.teammate_listen = teammate_config.listen;
            options.teammate_from_ini = true;
        }
        options.teammate_enabled = !options.teammate_off
                                   && (!options.teammate_peer.empty() || !options.teammate_listen.empty());
    }
    if (!options.teammate_enabled && !options.teammate_off) {
        // 什么都没给：默认就是 A 机，双击 exe 直接能用
        options.teammate_peer = kTeammateDefaultPeer;
        options.teammate_listen = kTeammateDefaultListen;
        options.teammate_enabled = true;
        options.teammate_from_default = true;
        if (options.teammate_role == 0) {
            options.teammate_role = 'A';
        }
    }

    ParseNetworkEndpoints(options);
    return options;
}

int RunApplication(platform::PlatformBackend &platform_backend, const LaunchOptions &options) {
    const auto main_start = std::chrono::steady_clock::now();
    SPDLOG_INFO("程序启动");
    SPDLOG_INFO("解析启动参数 mqtt_server={} video_port={} invert_udp_video={}",
                options.mqtt_server,
                options.video_port,
                options.invert_udp_video);
    SPDLOG_INFO("网络端点已解析 mqtt_host={} mqtt_port={} video_port={}",
                options.mqtt_host,
                options.mqtt_port,
                options.parsed_video_port);
    // 队友视角的来源说明（日志初始化之后才能打，所以放在这里而不是 ParseLaunchOptions 里）
    if (options.teammate_off) {
        SPDLOG_INFO("队友视角已在 {} 里关闭（peer=off），本次不占用任何端口", kTeammateConfigFileName);
    }
    if (options.teammate_ini_unreadable) {
        SPDLOG_WARN("发现 {} 但打不开（被占用/权限/路径），已忽略；本次用{}",
                    kTeammateConfigFileName,
                    options.teammate_from_command_line ? "命令行参数" : "内置默认 A 机");
    }
    if (options.teammate_from_command_line) {
        SPDLOG_INFO("队友视角参数来自命令行：peer={} listen={}",
                    options.teammate_peer,
                    options.teammate_listen);
    } else if (options.teammate_from_ini) {
        SPDLOG_INFO("队友视角参数来自 {}：peer={} listen={}",
                    kTeammateConfigFileName,
                    options.teammate_peer,
                    options.teammate_listen);
    } else if (options.teammate_from_default) {
        SPDLOG_INFO("队友视角使用内置默认（A 机）：peer={} listen={}；B 机在标题条上点「角色」切换",
                    options.teammate_peer,
                    options.teammate_listen);
    }

    if (!platform_backend.Initialize()) {
        return 1;
    }

    set_game_input_capture_handler([&platform_backend](const bool enabled) {
        platform_backend.SetGameInputCapture(enabled);
    });

    State state = CreateInitialState();

    // 队友角色：A / B。地址跟着它走，切换后写回 exe 同目录的 ini。
    std::string teammate_peer = options.teammate_peer;
    std::string teammate_listen = options.teammate_listen;
    char teammate_role = options.teammate_role != 0 ? options.teammate_role : 'A';
    bool teammate_role_from_default = options.teammate_from_default;

    auto mini_map_image = RoboGo::resource::LoadEmbeddedMapImage();
    if (!mini_map_image.has_value()) {
        SPDLOG_WARN("未能加载内嵌小地图，将回退为纯色小地图背景");
    } else if (platform_backend.UploadTexture(TextureSlot::MiniMap, *mini_map_image)) {
        SPDLOG_INFO("小地图纹理已上传 width={} height={}", mini_map_image->width, mini_map_image->height);
    } else {
        SPDLOG_ERROR("创建小地图纹理失败 width={} height={}", mini_map_image->width, mini_map_image->height);
    }

    {
        BackgroundServices background_services(options);
        std::chrono::steady_clock::time_point last_udp_frame_time = std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point last_custom_frame_time = std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point last_teammate_frame_time = std::chrono::steady_clock::time_point::min();
        bool prefer_custom_video_as_main = false;
        bool using_custom_video_as_main = false;
        // 队友画面的纯 UI 状态（未启用链路时永远保持默认值，不影响任何原有逻辑）
        bool teammate_collapsed = false;
        bool teammate_take_main_screen = false;
        float teammate_zoom = 1.0f;
        // 没启用链路时那一条折叠提示条：第一次启动打一次日志就够了，不要每帧刷。
        bool teammate_offline_hint_logged = false;
        // 纯观测计数器：只用于每秒一行日志，不参与任何控制决策。
        std::uint64_t udp_frame_count = 0;
        std::uint64_t custom_frame_count = 0;
        std::uint64_t teammate_frame_count = 0;
        std::uint64_t last_probe_udp_frames = 0;
        std::uint64_t last_probe_custom_frames = 0;
        std::size_t probe_loop_count = 0;
        auto last_probe_time = std::chrono::steady_clock::now();
        std::uint64_t last_probe_packets = 0;
        std::uint64_t last_probe_frames_out = 0;
        std::uint64_t last_probe_bytes = 0;

        const auto startup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - main_start)
                                        .count();
        SPDLOG_INFO("程序启动完成 startup_ms={}", startup_ms);

        while (platform_backend.ProcessEvents()) {
            background_services.ProcessMessages(state);
            background_services.DrainCustomVideoLogs(state);
            UpdateState(state);
            PublishCustomControlIfDue(state, background_services.mqtt_client());
            UploadLatestVideoFrames(platform_backend,
                                    background_services,
                                    last_udp_frame_time,
                                    last_custom_frame_time,
                                    last_teammate_frame_time,
                                    udp_frame_count,
                                    custom_frame_count,
                                    teammate_frame_count);

            // 视频探针：每秒一行，只读不干预。
            // 两路的「解出帧数」和「距上一帧多久」都打出来，用来区分三种情况：
            //   1) udp_custom_in 不涨 + custom_ago 一直涨  → 客户端根本没收到/没解出自定义流，问题在推流端或 MQTT 投递
            //   2) udp_custom_in 在涨 + custom_ago 很短     → 解码正常，问题在渲染/显示
            //   3) 两条一起崩                              → GPU 侧拖垮了整个循环
            // 注意 udp_ago / custom_ago 在从未收到过帧时是 -9223... 这种巨大的负数，表示“还没开始”，不是延迟。
            ++probe_loop_count;
            const auto probe_now = std::chrono::steady_clock::now();
            if (probe_now - last_probe_time >= std::chrono::seconds(1)) {
                const auto &decoder = background_services.custom_video_decoder();
                const std::uint64_t packets_total = decoder.packets_received();
                const std::uint64_t frames_out_total = decoder.frames_output();
                const std::uint64_t bytes_total = decoder.bytes_received();
                SPDLOG_INFO("视频探针 udp_in=+{} custom_in=+{} udp_total={} custom_total={} udp_ago={}ms custom_ago={}ms loop={}/s | cbb pkt_in=+{} pkt_taken_total={} frames_out=+{} bytes_in=+{}",
                            udp_frame_count - last_probe_udp_frames,
                            custom_frame_count - last_probe_custom_frames,
                            udp_frame_count,
                            custom_frame_count,
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                    probe_now - last_udp_frame_time).count(),
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                    probe_now - last_custom_frame_time).count(),
                            probe_loop_count,
                            packets_total - last_probe_packets,
                            decoder.packets_dequeued(),
                            frames_out_total - last_probe_frames_out,
                            bytes_total - last_probe_bytes);
                last_probe_time = probe_now;
                last_probe_udp_frames = udp_frame_count;
                last_probe_custom_frames = custom_frame_count;
                last_probe_packets = packets_total;
                last_probe_frames_out = frames_out_total;
                last_probe_bytes = bytes_total;
                probe_loop_count = 0;

                // 队友链路探针：同样只读。没启用时这一行根本不会出现。
                if (background_services.TeammateLinkEnabled()) {
                    const auto &link = background_services.TeammateStats();
                    const auto &video = background_services.TeammateVideoStatsSnapshot();
                    SPDLOG_INFO("队友探针 发={}帧/{}KB 丢={} 队列={} 连={} | 收={}帧/{}KB 连入={} | 解={}帧 重建={} 错误={} {}x{} peer={}",
                                link.frames_sent,
                                link.bytes_sent / 1024,
                                link.frames_dropped,
                                link.queued_now,
                                link.peer_connected ? "是" : "否",
                                link.frames_received,
                                link.bytes_received / 1024,
                                link.peer_served ? "是" : "否",
                                video.frames_decoded,
                                video.context_rebuilds,
                                video.decode_errors,
                                video.width,
                                video.height,
                                link.peer_address.empty() ? "-" : link.peer_address);
                }
            }

            if (!platform_backend.BeginFrame()) {
                continue;
            }

            const TextureView udp_video_texture = platform_backend.GetTextureView(TextureSlot::UdpVideo);
            const TextureView custom_video_texture = platform_backend.GetTextureView(TextureSlot::CustomVideo);
            const TextureView mini_map_texture = platform_backend.GetTextureView(TextureSlot::MiniMap);

            // 两档判据都要在 UpdateVideoLayoutPreference 之前算好：
            //   custom_video_display_ready —— 1 秒内有没有新帧（小窗/提示用，也是按 H 时的判据）
            //   custom_video_hold_as_main  —— 5 秒内有没有新帧（自动抢主画面用）
            // 只差这一档超时，就能让自定义流正常断包时主画面不来回切。
            const auto now = std::chrono::steady_clock::now();
            const bool udp_video_display_ready = IsVideoDisplayReady(
                    udp_video_texture,
                    last_udp_frame_time,
                    kUdpVideoFallbackTimeout,
                    now);
            const bool custom_video_display_ready = IsVideoDisplayReady(
                    custom_video_texture,
                    last_custom_frame_time,
                    kCustomVideoLiveTimeout,
                    now);
            const bool custom_video_hold_as_main = IsVideoDisplayReady(
                    custom_video_texture,
                    last_custom_frame_time,
                    kCustomVideoFallbackTimeout,
                    now);

            // 自动抢主画面只看「继续用」这一档（5 秒）；手动按 H 仍然按 1 秒那档给提示。
            UpdateVideoLayoutPreference(state,
                                        udp_video_display_ready,
                                        custom_video_display_ready,
                                        custom_video_hold_as_main,
                                        prefer_custom_video_as_main,
                                        using_custom_video_as_main);

            if (ImGui::IsKeyPressed(ImGuiKey_I, false) && !ImGui::GetIO().WantTextInput) {
                ToggleUdpVideoInversion(state, background_services);
            }

            // ---- 队友视角按键 ----
            // K = 折叠/展开小窗；L = 接管/退出主画面；L 模式下滚轮 1x~4x 放大。
            // 【只有链路真正启用时】才注册这些键：没启用时一个键都不碰，H/I/Esc 和发给机器人的键全部保持原样。
            // 全部避开 WASD/QE/RF/GZXCVB/Shift 这些会发给机器人的键，也绝不使用 CTRL（固件会复位整车）。
            TeammateViewState teammate_view;
            if (background_services.TeammateLinkEnabled()) {
                const bool typing = ImGui::GetIO().WantTextInput;
                if (ImGui::IsKeyPressed(ImGuiKey_K, false) && !typing) {
                    teammate_collapsed = !teammate_collapsed;
                    AppendSystemLog(state,
                                    0,
                                    teammate_collapsed ? "队友视角已折叠（按 K 展开）。"
                                                       : "队友视角已展开（按 K 折叠）。");
                }
                if (ImGui::IsKeyPressed(ImGuiKey_L, false) && !typing) {
                    teammate_take_main_screen = !teammate_take_main_screen;
                    teammate_zoom = 1.0f;
                    // 切进/切出都清掉在途帧，保证主画面一上来就是最新的一帧。
                    background_services.DropTeammateVideoFrames();
                    AppendSystemLog(state,
                                    0,
                                    teammate_take_main_screen ? "队友视角已接管主画面（按 L 切回，滚轮缩放）。"
                                                              : "主画面已切回本车视频。");
                }
                if (teammate_take_main_screen && !typing) {
                    const float wheel = ImGui::GetIO().MouseWheel;
                    if (wheel != 0.0f) {
                        teammate_zoom = ImClamp(teammate_zoom + wheel * 0.25f, 1.0f, 4.0f);
                        // 吃掉滚轮，免得同一格滚动把下面的日志面板也滚了
                        ImGui::GetIO().MouseWheel = 0.0f;
                    }
                }

                const TextureView teammate_texture = platform_backend.GetTextureView(TextureSlot::TeammateVideo);
                const auto teammate_now = std::chrono::steady_clock::now();
                const auto &link_stats = background_services.TeammateStats();
                const auto &video_stats = background_services.TeammateVideoStatsSnapshot();
                teammate_view.enabled = true;
                teammate_view.texture_ref = teammate_texture.texture_ref;
                teammate_view.texture_size = teammate_texture.size;
                teammate_view.display_ready = IsVideoDisplayReady(teammate_texture,
                                                                  last_teammate_frame_time,
                                                                  kTeammateVideoOfflineTimeout,
                                                                  teammate_now);
                teammate_view.live = IsVideoDisplayReady(teammate_texture,
                                                         last_teammate_frame_time,
                                                         kTeammateVideoLiveTimeout,
                                                         teammate_now);
                teammate_view.collapsed = teammate_collapsed;
                teammate_view.take_main_screen = teammate_take_main_screen;
                teammate_view.zoom = teammate_zoom;
                teammate_view.role = teammate_role;
                teammate_view.role_from_default = teammate_role_from_default;
                teammate_view.peer_connected = link_stats.peer_connected;
                teammate_view.peer_served = link_stats.peer_served;
                teammate_view.frame_inverted = link_stats.peer_frame_inverted;
                teammate_view.frames_sent = link_stats.frames_sent;
                teammate_view.frames_decoded = video_stats.frames_decoded;
                teammate_view.peer_address = link_stats.peer_address;
            } else {
                // 队友链路没建起来：要么 ini 里明确关掉了，要么参数解析失败。
                // 只打一次日志，不刷屏；K/L/滚轮照旧不注册。
                if (!teammate_offline_hint_logged) {
                    teammate_offline_hint_logged = true;
                    if (options.teammate_off) {
                        SPDLOG_INFO("队友视角已按 {} 的 peer=off 关闭（不出小窗，不占端口）",
                                    kTeammateConfigFileName);
                    } else {
                        SPDLOG_WARN("队友视角链路没有启用：检查 --teammate-* 或 exe 同目录的 {}",
                                    kTeammateConfigFileName);
                    }
                }
                teammate_view.enabled = false;
            }

            const MainWindowActions ui_actions = ShowMainWindow(
                    state,
                    background_services.mqtt_client(),
                    udp_video_texture.texture_ref,
                    udp_video_texture.size,
                    custom_video_texture.texture_ref,
                    custom_video_texture.size,
                    using_custom_video_as_main,
                    prefer_custom_video_as_main,
                    udp_video_display_ready,
                    custom_video_display_ready,
                    custom_video_hold_as_main,
                    background_services.IsUdpVideoInverted(),
                    mini_map_texture.texture_ref,
                    mini_map_texture.size,
                    teammate_view);
            if (ui_actions.toggle_udp_video_inversion) {
                ToggleUdpVideoInversion(state, background_services);
            }
            if (ui_actions.switch_teammate_role) {
                // 标题条上点了 [A]/[B]：切角色 = 换对方 IP，链路按新地址重连。
                ApplyTeammateRole(background_services,
                                  teammate_role,
                                  teammate_role_from_default,
                                  teammate_peer,
                                  teammate_listen,
                                  state);
            }

            update_keyboard_mouse_state(background_services.mqtt_client());

            ImGui::Render();
            platform_backend.RenderFrame(ImGui::GetDrawData());
        }

        SPDLOG_INFO("开始执行图形资源清理");
        platform_backend.Shutdown();

        SPDLOG_INFO("开始关闭网络与后台线程");
        background_services.Stop();
        background_services.DrainCustomVideoLogs(state);
        SPDLOG_INFO("程序退出完成");
    }

    set_game_input_capture_handler({});
    return 0;
}

}// namespace RoboGo::app
