#include "application.h"

#include "keyboard_mouse_state_manager.h"
#include "main_window.h"
#include "video/custom_byte_block_video_decoder_worker.h"
#include "video/rgba_image_transform.h"
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
          }),
          io_thread_([this]() {
              SPDLOG_INFO("IO线程已启动");
              io_context_.run();
              SPDLOG_INFO("IO线程已退出");
          }) {
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
    }

    [[nodiscard]] bool IsUdpVideoInverted() const {
        return udp_frame_converter_.IsInvertUdpVideoEnabled();
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

    void Stop() {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        mqtt_client_.stop();
        video_stream_manager_.stop();
        io_context_.stop();
        if (io_thread_.joinable()) {
            io_thread_.join();
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
    moodycamel::ReaderWriterQueue<UiLogMessage> custom_video_log_queue_;
    AvFrameRgbaConverter udp_frame_converter_;
    RoboGo::video::CustomByteBlockVideoDecoderWorker custom_byte_block_decoder_worker_;
    RoboGo::io::MqttClient mqtt_client_;
    RoboGo::video::VideoStreamManager video_stream_manager_;
    std::thread io_thread_;
    bool stopped_ = false;
};

void UploadLatestVideoFrames(RoboGo::platform::PlatformBackend &platform_backend,
                             BackgroundServices &background_services,
                             std::chrono::steady_clock::time_point &last_udp_frame_time,
                             std::chrono::steady_clock::time_point &last_custom_frame_time) {
    const auto video_frame = background_services.TakeLatestVideoFrame();
    if (video_frame.has_value()) {
        last_udp_frame_time = std::chrono::steady_clock::now();
        if (!platform_backend.UploadTexture(TextureSlot::UdpVideo, *video_frame)) {
            SPDLOG_ERROR("创建UDP视频纹理失败 width={} height={}", video_frame->width, video_frame->height);
        }
    }

    const auto custom_video_frame = background_services.TakeLatestCustomVideoFrame();
    if (custom_video_frame.has_value()) {
        last_custom_frame_time = std::chrono::steady_clock::now();
        if (!platform_backend.UploadTexture(TextureSlot::CustomVideo, *custom_video_frame)) {
            SPDLOG_ERROR("创建 CustomByteBlock 视频纹理失败 width={} height={}",
                         custom_video_frame->width,
                         custom_video_frame->height);
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

    if (!platform_backend.Initialize()) {
        return 1;
    }

    set_game_input_capture_handler([&platform_backend](const bool enabled) {
        platform_backend.SetGameInputCapture(enabled);
    });

    State state = CreateInitialState();

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
        bool prefer_custom_video_as_main = false;
        bool using_custom_video_as_main = false;

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
                                    last_custom_frame_time);

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

            const bool toggle_udp_video_inversion_requested = ShowMainWindow(
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
                    background_services.IsUdpVideoInverted(),
                    mini_map_texture.texture_ref,
                    mini_map_texture.size);
            if (toggle_udp_video_inversion_requested) {
                ToggleUdpVideoInversion(state, background_services);
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
