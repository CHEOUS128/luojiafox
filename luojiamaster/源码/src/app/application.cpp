#include "video/rgba_frame_converter.h"
#include "application.h"
#include "app/ul_live_view.h"
#include "app/local_session_replay.h"
#include "app/local_peer_replay.h"
#include "platform/platform_util.h"
#include "app/trial_match.h"
#include "panel/start_panel.h"
#include <charconv>
#include "embedded_ul_map.h"
#include "embedded_uc_commander_map.h"
#include "panel/commander_panel.h"
#include "app/commander_benchmark.h"
#include "panel/cup_map_replay.h"
#include "app/cup_layout_benchmark.h"
#include "app/latest_video_frame.h"

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

std::string DefaultMqttServer() {
    // return "tcp://127.0.0.1:3333";// 模拟裁判系统
    return "tcp://192.168.12.1:3333";// 正式比赛
    // return "tcp://192.168.1.2:3333";// 自己部署的裁判系统
}

constexpr const char *kTeammateConfigFileName = "队友视角.ini";
constexpr std::array kPeerTextureSlots{TextureSlot::TeammateVideo, TextureSlot::TeammateVideo2, TextureSlot::TeammateVideo3};
using PeerTimes = std::array<std::chrono::steady_clock::time_point, kPeerTextureSlots.size()>;

struct TeammateConfig {
    bool unreadable = false;
    std::string peer;
    std::string listen;
};

[[nodiscard]] TeammateConfig LoadTeammateConfig() {
    TeammateConfig config;
    using namespace RoboGo::platform_util;
    const auto path = ExecutableDirectory() / PathFromUtf8(kTeammateConfigFileName);
    std::error_code error;
    if (!std::filesystem::exists(path, error)) return config;

    auto stream = OpenBinaryInput(PathToUtf8(path));
    if (!stream) {
        config.unreadable = true;
        return config;
    }
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
        }
    }
    return config;
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
    explicit AvFrameRgbaConverter(bool inverted) : invert_udp_video_(inverted) {}
    std::optional<VideoFrame> convert(const AVFrame *frame) { return converter_.Convert(frame); }
    void SetInvertUdpVideo(bool value) { invert_udp_video_.store(value, std::memory_order_relaxed); }
    [[nodiscard]] bool IsInvertUdpVideoEnabled() const { return invert_udp_video_.load(std::memory_order_relaxed); }
private:
    RoboGo::video::RgbaFrameConverter converter_;
    std::atomic<bool> invert_udp_video_ = false;
};

// The network producer is destroyed before the decoder, and the decoder before its mailbox.
struct PeerChannel {
    PeerChannel(int id, std::string name, bool reuse_aligned_rgba = false)
        : robot_id(id), label(std::move(name)),
          decoder({.reuse_aligned_rgba = reuse_aligned_rgba}, [this](VideoFrame &&frame) { mailbox.Push(std::move(frame)); }) {}
    int robot_id;
    std::string label;
    bool paused = false;
    RoboGo::app::LatestVideoFrameMailbox<VideoFrame> mailbox;
    RoboGo::video::TeammateVideoReceiver decoder;
    std::unique_ptr<RoboGo::net::TeammateLink> link;
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
                      custom_video_frame_queue_.Push(std::move(custom_video_frame));
                  },
                  [this](const int type, const std::string &text) {
                      UiLogMessage log_message;
                      log_message.type = type;
                      log_message.text = text;
                      std::lock_guard lock(custom_video_log_mutex_);
                      if (custom_video_log_queue_.size() >= 128) custom_video_log_queue_.pop_front();
                      custom_video_log_queue_.push_back(std::move(log_message));
                  }) {
        const auto on_frame = [this](const AVFrame *frame) {
            if (auto image = udp_frame_converter_.convert(frame)) {
                video_frame_queue_.Push(std::move(*image));
            } else {
                SPDLOG_WARN("官方视频帧转换失败");
            }
        };
        if (options.local_trial) {
            mqtt_client_ = std::make_unique<RoboGo::io::MqttClient>(
                [this, cup = options.competition](const std::string &topic, const std::string &payload) {
                    const std::lock_guard lock(trial_writer_mutex_);
                    if (cup == RoboGo::app::Competition::Uc2027) {
                        cup_seat_.Receive(topic, payload);
                    } else if (cup == RoboGo::app::Competition::Ul2027) {
                        if (auto reply = trial_loadout_.Reply(topic, payload)) {
                            OnMessage("RobotPerformanceSelectionSync", *reply);
                        }
                    }
                });
            video_stream_manager_ = std::make_unique<RoboGo::video::VideoStreamManager>(on_frame);
            replay_ = std::make_unique<RoboGo::app::LocalSessionReplay>(
                [this](unsigned lane, RoboGo::app::local_capture::Bytes bytes) {
                    if (lane == 0) {
                        video_stream_manager_->receive_datagram(bytes.data(), bytes.size());
                    } else {
                        OnMessage("CustomByteBlock", std::string(
                            reinterpret_cast<const char *>(bytes.data()), bytes.size()));
                    }
                },
                [this]() {
                    video_stream_manager_->restart_recording();
                    custom_byte_block_decoder_worker_.restart_recording();
                });
            SPDLOG_INFO("本地测试：双路录像就绪，等待选择身份；网络服务未启动");
            return;
        }

        mqtt_client_ = std::make_unique<RoboGo::io::MqttClient>(io_context_, options.mqtt_host,
            options.mqtt_port, [this](const auto &topic, const auto &payload) { OnMessage(topic, payload); });
        video_stream_manager_ = std::make_unique<RoboGo::video::VideoStreamManager>(io_context_,
            boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), options.parsed_video_port),
            on_frame, [this](const std::vector<uint8_t> &frame) {
                const std::lock_guard lock(peer_publish_mutex_);
                for (const auto &channel : peers_) {
                    if (channel->link) channel->link->push_frame(frame);
                }
            });
        // Every object captured by an IO/decode callback is fully constructed
        // before the IO context can deliver the first network packet.
        io_thread_ = std::thread([this]() {
            SPDLOG_INFO("IO线程已启动");
            try {
                io_context_.run();
            } catch (const std::exception &error) {
                SPDLOG_ERROR("IO线程异常，网络接收已停止 message={}", error.what());
            } catch (...) {
                SPDLOG_ERROR("IO线程出现未知异常，网络接收已停止");
            }
            SPDLOG_INFO("IO线程已退出");
        });
    }

    ~BackgroundServices() {
        Stop();
    }

    BackgroundServices(const BackgroundServices &) = delete;
    BackgroundServices &operator=(const BackgroundServices &) = delete;

    RoboGo::io::MqttClient &mqtt_client() {
        return *mqtt_client_;
    }

    void SetUdpVideoInverted(const bool enabled) {
        udp_frame_converter_.SetInvertUdpVideo(enabled);
        // 队友那一路是原样透传的字节，翻转状态得跟着一起同步，
        // 否则队友画面的朝向会和我们主画面不一致。
        for (const auto &channel : peers_) {
            if (channel->link) channel->link->set_local_inverted(enabled);
        }
    }

    [[nodiscard]] bool IsUdpVideoInverted() const {
        return udp_frame_converter_.IsInvertUdpVideoEnabled();
    }

    // Only the UI thread replaces sessions. The UDP worker may be publishing a frame
    // concurrently, so detach the old channels under a short lock and join outside it.
    void SelectPeerIdentity(const LaunchOptions &options, int login_id) {
        StopPeers();
        if ((!options.local_trial && options.teammate_off) || login_id == 0) return;
        auto ids = RoboGo::net::TeamSeatIds(options.competition, login_id);
        const bool manual = !options.local_trial &&
                            (!options.teammate_peer.empty() || !options.teammate_listen.empty());
        if (manual && ids.size() > 1) ids.resize(1);
        if (ids.empty()) return;

        decltype(peers_) next;
        for (const int remote_id : ids) {
            auto channel = std::make_unique<PeerChannel>(remote_id,
                manual ? "手工地址" : RoboGo::net::TeamSeatName(options.competition, remote_id));
            if (!options.local_trial) {
                RoboGo::net::TeammateLinkConfig config;
                if (manual) {
                    std::string error;
                    if (!options.teammate_peer.empty() &&
                        !RoboGo::net::TeammateLink::ParseEndpoint(options.teammate_peer, config.peer, error)) {
                        SPDLOG_ERROR("队友手工地址无效 address={} error={}", options.teammate_peer, error);
                        return;
                    }
                    if (!options.teammate_listen.empty()) {
                        const auto &port = options.teammate_listen;
                        const auto result = std::from_chars(port.data(), port.data() + port.size(), config.listen_port);
                        if (result.ec != std::errc{} || result.ptr != port.data() + port.size() || config.listen_port == 0) {
                            SPDLOG_ERROR("队友手工监听端口无效 port={}", port);
                            return;
                        }
                    }
                } else {
                    config = RoboGo::net::TeamSeatLinkConfig(options.competition, login_id, remote_id);
                }
                channel->link = std::make_unique<RoboGo::net::TeammateLink>(std::move(config),
                    [target = channel.get()](std::vector<std::uint8_t> &&frame) {
                        target->decoder.submit_frame(std::move(frame));
                    });
                channel->link->set_local_inverted(IsUdpVideoInverted());
            }
            next.push_back(std::move(channel));
        }
        {
            const std::lock_guard lock(peer_publish_mutex_);
            peers_.swap(next);
        }
        if (options.local_trial) {
            StartPeerReplay(options.cup_layout_benchmark ? options.commander_video_dir : std::string{});
        }
        SPDLOG_INFO("队友视角已按登录身份建立 本机={} 队友数={} 来源={}", login_id, peers_.size(),
                    options.local_trial ? "本地测试" : manual ? "手工地址" : "岗位专线");
    }

    void SelectCommanderPeers(const LaunchOptions &options, int team) {
        if (!options.local_trial || options.competition != RoboGo::app::Competition::Uc2027 ||
            (team != 0 && team != 1)) throw std::logic_error("Invalid local commander seat");
        StopPeers();
        for (const int seat : {1, 3, 4}) {
            const int id = seat + (team == 1 ? 100 : 0);
            peers_.push_back(std::make_unique<PeerChannel>(id, RoboGo::net::TeamSeatName(options.competition, id), true));
        }
        StartPeerReplay(options.commander_video_dir);
        SPDLOG_INFO("本地指挥席 三路录像已启动 team={} source={}", team, options.commander_video_dir.empty() ? "embedded" : options.commander_video_dir);
    }

    void ToggleReplayPeer(std::size_t slot) {
        if (!local_peers_ || slot >= peers_.size()) return;
        auto &channel = peers_[slot];
        if (channel->paused) {
            // Replace the stopped decoder while that producer is parked. Its
            // next input begins with a keyframe and its mailbox starts empty.
            channel = std::make_unique<PeerChannel>(channel->robot_id, channel->label, peers_.size() == 3);
            local_peers_->SetPaused(slot, false);
        } else {
            local_peers_->SetPaused(slot, true);
            channel->paused = true;
            channel->decoder.stop();
            channel->mailbox.Clear();
        }
        SPDLOG_INFO("本地录像 队友={} paused={}", channel->robot_id, channel->paused);
    }

    [[nodiscard]] const auto &Peers() const { return peers_; }

    void OnMessage(const std::string &topic, const std::string &payload) {
        if (topic == "CustomByteBlock") {
            CustomByteBlock message;
            if (!message.ParseFromString(payload)) {
                SPDLOG_WARN("CustomByteBlock protobuf parse failed bytes={}", payload.size());
                return;
            }
            const auto &bytes = message.data();
            custom_byte_block_decoder_worker_.enqueue_packet_bytes(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()));
        } else if (!message_queue_.try_emplace(topic, payload, std::chrono::steady_clock::now())) {
            messages_dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void StartLocalReplay(std::chrono::steady_clock::time_point start,
                          RoboGo::app::Competition cup, int login_id) {
        if (!replay_) return;
        RoboGo::app::LocalSessionReplay::MatchTick tick;
        if (cup != RoboGo::app::Competition::Uc2026) {
            tick = [this, cup, login_id](int second) {
                const std::lock_guard lock(trial_writer_mutex_);
                const auto emit = [this](const auto &topic, const auto &payload) { OnMessage(topic, payload); };
                if (cup == RoboGo::app::Competition::Uc2027) {
                    cup_seat_.Tick(login_id, second, emit);
                    return;
                }
                trial_loadout_.Advance(login_id, second);
                RoboGo::app::EmitTrialMatch(cup, login_id, second, emit, trial_loadout_.chassis);
            };
        }
        replay_->Start(start, std::move(tick));
    }

    void ProcessMessages(State &state) {
        QueuedMessage message;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
        for (std::size_t handled = 0; handled < 128 && std::chrono::steady_clock::now() < deadline
                                      && message_queue_.try_dequeue(message); ++handled) {
            HandleMqttMessage(state, *mqtt_client_, message.topic, message.payload, {}, message.received_at);
        }
    }

    void DrainCustomVideoLogs(State &state) {
        std::deque<UiLogMessage> messages;
        {
            std::lock_guard lock(custom_video_log_mutex_);
            for (std::size_t count = 0; count < 32 && !custom_video_log_queue_.empty(); ++count) {
                messages.push_back(std::move(custom_video_log_queue_.front()));
                custom_video_log_queue_.pop_front();
            }
        }
        for (const auto &message : messages) {
            AppendVehicleLog(state, message.type, message.text);
        }
    }

    void LogVideoQueueStats() const {
        const auto udp = video_frame_queue_.Stats();
        const auto custom = custom_video_frame_queue_.Stats();
        std::size_t peer_pending = 0;
        std::uint64_t peer_overwritten = 0;
        for (const auto &channel : peers_) {
            const auto stats = channel->mailbox.Stats();
            peer_pending += stats.pending;
            peer_overwritten += stats.overwritten;
        }
        SPDLOG_INFO("视频缓冲 pending={}/{}/{} covered={}/{}/{} mqtt_dropped={} cbb_queued={} cbb_dropped={}",
                    udp.pending, custom.pending, peer_pending,
                    udp.overwritten, custom.overwritten, peer_overwritten,
                    messages_dropped_.load(std::memory_order_relaxed),
                    custom_byte_block_decoder_worker_.queued_packets(),
                    custom_byte_block_decoder_worker_.packets_dropped());
    }

    std::optional<VideoFrame> TakeLatestVideoFrame() {
        return video_frame_queue_.Take();
    }

    std::optional<VideoFrame> TakeLatestCustomVideoFrame() {
        return custom_video_frame_queue_.Take();
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
        replay_.reset(); // Join the recording producer before stopping either receive queue.
        mqtt_client_->stop();
        StopPeers();
        video_stream_manager_->stop();
        io_context_.stop();
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
        custom_byte_block_decoder_worker_.stop();
        video_frame_queue_.Clear();
        custom_video_frame_queue_.Clear();
    }

private:
    void StartPeerReplay(const std::string &video_dir) {
        std::vector<int> seats;
        for (const auto &peer : peers_) seats.push_back(peer->robot_id);
        local_peers_ = std::make_unique<RoboGo::app::LocalPeerReplay>(seats,
            [this](std::size_t slot, std::vector<std::uint8_t> &&frame) {
                peers_[slot]->decoder.submit_frame(std::move(frame));
            }, video_dir);
    }

    void StopPeers() {
        local_peers_.reset();
        decltype(peers_) previous;
        {
            const std::lock_guard lock(peer_publish_mutex_);
            peers_.swap(previous);
        }
        // Joins each link before releasing its decoder and mailbox.
    }

    boost::asio::io_context io_context_;
    struct QueuedMessage {
        std::string topic;
        std::string payload;
        std::chrono::steady_clock::time_point received_at;
    };
    moodycamel::ReaderWriterQueue<QueuedMessage> message_queue_{2048};
    // Serialize local referee settings and the two local writers to the SPSC queue.
    std::mutex trial_writer_mutex_;
    RoboGo::app::UlTrialLoadout trial_loadout_;
    RoboGo::app::CupTrialSeat cup_seat_;
    std::atomic<std::uint64_t> messages_dropped_{0};
    RoboGo::app::LatestVideoFrameMailbox<VideoFrame> video_frame_queue_;
    RoboGo::app::LatestVideoFrameMailbox<VideoFrame> custom_video_frame_queue_;
    std::mutex custom_video_log_mutex_;
    std::deque<UiLogMessage> custom_video_log_queue_;
    AvFrameRgbaConverter udp_frame_converter_;
    RoboGo::video::CustomByteBlockVideoDecoderWorker custom_byte_block_decoder_worker_;
    std::unique_ptr<RoboGo::io::MqttClient> mqtt_client_;
    std::unique_ptr<RoboGo::video::VideoStreamManager> video_stream_manager_;
    std::unique_ptr<RoboGo::app::LocalSessionReplay> replay_;
    std::mutex peer_publish_mutex_;
    std::vector<std::unique_ptr<PeerChannel>> peers_;
    std::unique_ptr<RoboGo::app::LocalPeerReplay> local_peers_;
    std::thread io_thread_;
    bool stopped_ = false;
};

struct PendingVideoPresentation {
    bool udp = false;
    bool custom = false;
    std::array<bool, kPeerTextureSlots.size()> peers{};
};

PendingVideoPresentation UploadLatestVideoFrames(RoboGo::platform::PlatformBackend &platform_backend,
                                                 BackgroundServices &background_services) {
    PendingVideoPresentation pending;
    const auto video_frame = background_services.TakeLatestVideoFrame();
    if (video_frame.has_value()) {
        pending.udp = platform_backend.UploadTexture(TextureSlot::UdpVideo, *video_frame);
    }
    const auto custom_video_frame = background_services.TakeLatestCustomVideoFrame();
    if (custom_video_frame.has_value()) {
        pending.custom = platform_backend.UploadTexture(TextureSlot::CustomVideo, *custom_video_frame);
    }
    const auto &peers = background_services.Peers();
    for (std::size_t slot = 0; slot < peers.size(); ++slot) {
        if (const auto frame = peers[slot]->mailbox.Take()) {
            pending.peers[slot] = platform_backend.UploadTexture(kPeerTextureSlots[slot], *frame);
        }
    }
    return pending;
}

void CommitVideoPresentation(const PendingVideoPresentation &pending,
                             std::chrono::steady_clock::time_point &last_udp_frame_time,
                             std::chrono::steady_clock::time_point &last_custom_frame_time,
                             PeerTimes &last_peer_frames,
                             std::uint64_t &udp_frame_count,
                             std::uint64_t &custom_frame_count) {
    const auto now = std::chrono::steady_clock::now();
    if (pending.udp) {
        last_udp_frame_time = now;
        ++udp_frame_count;
    }
    if (pending.custom) {
        last_custom_frame_time = now;
        ++custom_frame_count;
    }
    for (std::size_t slot = 0; slot < pending.peers.size(); ++slot) {
        if (pending.peers[slot]) last_peer_frames[slot] = now;
    }
}

[[nodiscard]] bool IsVideoDisplayReady(const TextureView &video_texture,
                                       const std::chrono::steady_clock::time_point last_frame_time,
                                       const std::chrono::steady_clock::duration fallback_timeout,
                                       const std::chrono::steady_clock::time_point now,
                                       bool just_uploaded = false) {
    return RoboGo::app::CanDrawVideoFrame(video_texture.IsReady(), just_uploaded, last_frame_time,
                                          fallback_timeout, now);
}

void UpdateVideoLayoutPreference(State &state, bool allow_shortcuts,
                                 const bool udp_video_display_ready,
                                 const bool custom_video_display_ready,
                                 const bool custom_video_hold_as_main,
                                 bool &prefer_custom_video_as_main,
                                 bool &using_custom_video_as_main) {
    bool handled_manual_video_layout_toggle = false;
    if (allow_shortcuts && ImGui::IsKeyPressed(ImGuiKey_H, false) && !ImGui::GetIO().WantTextInput) {
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
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--mqtt_server" && i + 1 < args.size()) {
            options.mqtt_server = args[i + 1];
            ++i;
        } else if (args[i] == "--video_port" && i + 1 < args.size()) {
            options.video_port = args[i + 1];
            ++i;
        } else if (args[i] == "--test-display-1080") {
            // Reuse the benchmark monitor choice for interactive fullscreen layout checks.
            options.benchmark_match_monitor = true;
        } else if (args[i] == "--cup-layout-benchmark") {
            options.cup_layout_benchmark = true;
        } else if (args[i] == "--commander-benchmark-1080-display") {
            options.benchmark_match_monitor = true;
            options.commander_benchmark = true;
        } else if (args[i] == "--commander-benchmark") {
            options.commander_benchmark = true;
        } else if (args[i] == "--commander-marker-benchmark") {
            options.commander_benchmark = true;
            options.commander_comparison = CommanderComparison::Markers;
        } else if (args[i] == "--commander-layout-benchmark") {
            options.commander_benchmark = true;
            options.commander_comparison = CommanderComparison::Layouts;
        } else if (args[i] == "--commander-video-dir") {
            if (i + 1 >= args.size() || args[i + 1].empty() || args[i + 1].starts_with("--"))
                throw std::invalid_argument("Missing --commander-video-dir directory");
            options.commander_video_dir = args[++i];
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

    if (options.teammate_peer.empty() && options.teammate_listen.empty()) {
        const auto config = LoadTeammateConfig();
        options.teammate_ini_unreadable = config.unreadable;
        options.teammate_peer = config.peer;
        if (!config.peer.empty() || config.listen != "47700") options.teammate_listen = config.listen;
    }
    std::string peer = options.teammate_peer;
    for (char &c : peer) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    options.teammate_off = peer == "off" || peer == "0";

    ParseNetworkEndpoints(options);
    return options;
}

int RunApplication(platform::PlatformBackend &platform_backend, const LaunchOptions &options) {
    const auto main_start = std::chrono::steady_clock::now();
    SPDLOG_INFO("程序启动");
    SPDLOG_INFO("客户端赛事={} 数据来源={}", options.competition == Competition::Ul2027 ?
        "2027 RMUL" : options.competition == Competition::Uc2026 ? "2026 RMUC" : "2027 RMUC",
        options.local_trial ? "本地测试" : "正式登录");
    if (!options.local_trial) {
        SPDLOG_INFO("解析启动参数 mqtt_server={} video_port={} invert_udp_video={}",
                    options.mqtt_server,
                    options.video_port,
                    options.invert_udp_video);
        SPDLOG_INFO("网络端点已解析 mqtt_host={} mqtt_port={} video_port={}",
                    options.mqtt_host,
                    options.mqtt_port,
                    options.parsed_video_port);
        if (options.teammate_ini_unreadable) SPDLOG_WARN("无法读取 {}，使用登录岗位", kTeammateConfigFileName);
        if (options.teammate_off) SPDLOG_INFO("队友专线已关闭（peer=off）");
    }

    const bool alliance = options.competition == Competition::Ul2027;
    const bool commander_allowed = options.local_trial && options.competition == Competition::Uc2027;
    if (options.cup_layout_benchmark && (!commander_allowed || options.commander_benchmark || options.commander_video_dir.empty())) {
        SPDLOG_ERROR("操作手地图对比需要 2027 超级对抗赛本地测试及录像目录");
        return 1;
    }
    if (options.commander_benchmark && (!commander_allowed || options.commander_video_dir.empty())) {
        SPDLOG_ERROR("指挥席性能测试需要 2027 超级对抗赛本地模式及三路压力录像目录");
        return 1;
    }
    if (!options.commander_video_dir.empty() && !commander_allowed) {
        SPDLOG_ERROR("指挥席录像目录仅供 2027 超级对抗赛本地测试使用");
        return 1;
    }
    platform_backend.PreferPerformanceAdapter(commander_allowed);
    platform_backend.RequestBenchmarkMonitor(options.benchmark_match_monitor);
    platform_backend.SetDesktopWindowKind(alliance ? platform::DesktopWindowKind::League
                                                   : platform::DesktopWindowKind::Cup);
    if (!platform_backend.Initialize()) {
        return 1;
    }

    reset_client_session_input();
    ResetStartPanelSession(options.competition);
    // In a local trial the UI can still enter/exit the match view. Keep the
    // desktop cursor free and never acquire hardware game-input capture.
    if (!options.local_trial && !alliance) {
        set_game_input_capture_handler([&platform_backend](bool enabled) {
            platform_backend.SetGameInputCapture(enabled);
        });
    } else {
        set_game_input_capture_handler({});
    }

    State state = CreateInitialState();
    state.competition = options.competition;
    int session_result = 0;

    TextureSlot map_slot = TextureSlot::MiniMap;
    std::optional<resource::DecodedImage> map_image;
    switch (options.competition) {
        case Competition::Uc2026:
            map_image = resource::LoadEmbeddedMapImage();
            break;
        case Competition::Ul2027:
            map_slot = TextureSlot::UlMap;
            map_image = resource::LoadEmbeddedImage({embedded_ul_map_data, embedded_ul_map_data_len}, "2027 高校联盟赛地图");
            break;
        case Competition::Uc2027:
            map_slot = TextureSlot::UcCommanderFlat;
            map_image = resource::LoadEmbeddedImage({embedded_uc_commander_map_data, embedded_uc_commander_map_data_len}, "2027 超级对抗赛地图");
            break;
    }
    if (!map_image) SPDLOG_WARN("场地图未能加载");

    {
        BackgroundServices background_services(options);
        UlHudView hud;
        bool commander_active = false;
        CommanderDeskState commander_state;
        std::optional<CupMapReplay> cup_replay;
        std::optional<uint32_t> cup_replay_round;
        std::optional<CupLayoutBenchmark> cup_benchmark;
        if (options.cup_layout_benchmark) {
            cup_benchmark.emplace();
            background_services.mqtt_client().set_client_id("103");
        }
        std::optional<CommanderBenchmark> benchmark;
        if (options.commander_benchmark) benchmark.emplace(options.commander_comparison);
        bool trial_started = false;
        int trial_second = -1;
        auto trial_start = std::chrono::steady_clock::time_point{};
        std::chrono::steady_clock::time_point last_udp_frame_time = std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point last_custom_frame_time = std::chrono::steady_clock::time_point::min();
        PeerTimes last_peer_frames;
        last_peer_frames.fill(std::chrono::steady_clock::time_point::min());
        bool prefer_custom_video_as_main = false;
        bool using_custom_video_as_main = false;
        int peer_identity = 0;
        bool teammates_collapsed = false;
        bool teammate_overview = false;
        TeammatePointerGuard peer_input;
        int focused_peer = -1;
        float teammate_zoom = 1.0f;
        // 纯观测计数器：只用于每秒一行日志，不参与任何控制决策。
        std::uint64_t udp_frame_count = 0;
        std::uint64_t custom_frame_count = 0;
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

        using Clock = std::chrono::steady_clock;
        auto last_present = Clock::now(), previous_slow_log = last_present;
        while (platform_backend.ProcessEvents()) {
            auto &client = background_services.mqtt_client();
            const auto conn = client.get_connection_state();
            using Connection = RoboGo::io::MqttClient::ConnectionState;
            // Trigger once per explicit login, not on transient reconnects.
            const auto id = client.get_client_id();
            int login_id = 0;
            if (!id.empty()) {
                const auto result = std::from_chars(id.data(), id.data()+id.size(), login_id);
                if (result.ec != std::errc{} || result.ptr != id.data()+id.size()) login_id = 0;
            }
            const int connected_identity = conn == Connection::Connected ? login_id : 0;
            if (!commander_active && peer_identity != connected_identity) {
                background_services.SelectPeerIdentity(options, connected_identity);
                peer_identity = connected_identity;
                last_peer_frames.fill(std::chrono::steady_clock::time_point::min());
                teammates_collapsed = teammate_overview = false;
                focused_peer = -1;
                teammate_zoom = 1.0f;
            }
            if (!commander_active && options.local_trial && conn == Connection::Connected && login_id > 0) {
                if (!trial_started) {
                    trial_started = true;
                    trial_start = std::chrono::steady_clock::now();
                    background_services.StartLocalReplay(trial_start, options.competition, login_id);
                    if (!alliance && IsCupMapSeat(login_id)) enter_game_mode();
                    SPDLOG_INFO("本地测试开始 robot_id={}", login_id);
                }
                if (options.competition == Competition::Uc2026) {
                    const int seconds = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - trial_start).count());
                    if (seconds != trial_second) {
                        trial_second = seconds;
                        EmitTrialMatch(options.competition, login_id, seconds,
                            [&](const auto &topic, const auto &payload) { background_services.OnMessage(topic, payload); });
                    }
                }
            } else if (commander_active) {
                // The local spectator receives only the public match clock, without a robot login.
                const int seconds = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                    Clock::now() - trial_start).count());
                if (seconds != trial_second) {
                    trial_second = seconds;
                    background_services.OnMessage("GameStatus", CupTrialClock(seconds).SerializeAsString());
                }
            } else if (options.local_trial && trial_started) {
                session_result = kReturnToLauncher;
                break;
            }
            background_services.ProcessMessages(state);
            background_services.DrainCustomVideoLogs(state);
            UpdateState(state);
            const bool alliance_signed_in = alliance && conn == Connection::Connected && login_id > 0;
            const bool cup_seat = !alliance && !commander_active && IsCupMapSeat(connected_identity);
            const bool identity_changed = hud.SelectIdentity(alliance_signed_in || cup_seat ? connected_identity : 0);
            if (identity_changed) {
                if (alliance) last_udp_frame_time = last_custom_frame_time = std::chrono::steady_clock::time_point::min();
                if (cup_seat && options.local_trial) cup_replay.emplace();
                else cup_replay.reset();
                cup_replay_round.reset();
            }
            if (cup_replay) {
                const auto round = state.game_status ? state.game_status->current_round() : 0u;
                if (cup_replay_round != round) {
                    cup_replay->Reset(connected_identity, Clock::now());
                    cup_replay_round = round;
                    const auto &peers = background_services.Peers();
                    for (std::size_t slot = 0; slot < peers.size(); ++slot) {
                        if (peers[slot]->paused) {
                            background_services.ToggleReplayPeer(slot);
                            last_peer_frames[slot] = Clock::time_point::min();
                        }
                    }
                }
            }
            if (!alliance && !options.local_trial) PublishCustomControlIfDue(state, client);
            if (!platform_backend.BeginFrame()) continue;
            const bool menu_open = alliance ? hud.menu_open : !is_in_game_mode();
            const auto &peer_channels = background_services.Peers();
            const auto &io = ImGui::GetIO();
            const bool escape_pressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
            const bool map_was_open = hud.map_open;
            if (cup_benchmark && cup_replay) {
                hud.map_open = cup_benchmark->Map();
                teammate_overview = !hud.map_open;
                focused_peer = -1;
            }
            const bool map_owns_input = hud.HandleMapKeys(
                io.KeyMods == 0 && ImGui::IsKeyPressed(ImGuiKey_M, false), escape_pressed, io.WantTextInput,
                menu_open || is_marker_mode());
            if (map_was_open != hud.map_open)
                SPDLOG_INFO("操作手二维大图 {} seat={}", hud.map_open ? "打开" : "关闭", hud.identity);
            const bool peer_keys = !commander_active && !map_owns_input && !peer_channels.empty() && !menu_open && !io.WantTextInput;
            if (peer_keys && ImGui::IsKeyPressed(ImGuiKey_K, false) && !teammate_overview)
                teammates_collapsed = !teammates_collapsed;
            if (peer_keys && ImGui::IsKeyPressed(ImGuiKey_L, false)) {
                teammate_overview = !teammate_overview;
                focused_peer = -1;
                teammate_zoom = 1.0f;
            }
            const bool leaving_overview_with_esc = !map_owns_input && (!alliance || !menu_open) && teammate_overview && escape_pressed;
            if (leaving_overview_with_esc) {
                teammate_overview = false;
                focused_peer = -1;
                teammate_zoom = 1.0f;
                if (!alliance) reset_client_session_input();
            }
            TeammatePointerGuard::Step peer_input_step;
            if (alliance_signed_in && !map_owns_input && !leaving_overview_with_esc && escape_pressed) {
                hud.menu_open = !hud.menu_open;
            } else if (!commander_active && !alliance) {
                peer_input_step = peer_input.Advance(teammate_overview || map_owns_input, any_match_controls_held());
                if (!options.local_trial) {
                    if (peer_input_step.release_cursor) platform_backend.SetGameInputCapture(false);
                    if (peer_input_step.restore_cursor)
                        platform_backend.SetGameInputCapture(is_in_game_mode() && connected_identity != 0);
                }
                update_keyboard_mouse_state(client, peer_input_step.sample);
            }
            const auto upload_begin = benchmark ? Clock::now() : Clock::time_point{};
            const auto pending_video_presentation = UploadLatestVideoFrames(platform_backend, background_services);

            const auto upload_end = benchmark ? Clock::now() : Clock::time_point{};
            // Video probe counts successfully presented new textures. Compare
            // frames_out and queue counters to distinguish decode/display stalls.
            // -1 means this stream has not submitted an image yet.
            ++probe_loop_count;
            const auto probe_now = std::chrono::steady_clock::now();
            if (probe_now - last_probe_time >= std::chrono::seconds(1)) {
                background_services.LogVideoQueueStats();
                const auto &decoder = background_services.custom_video_decoder();
                const std::uint64_t packets_total = decoder.packets_received();
                const std::uint64_t frames_out_total = decoder.frames_output();
                const std::uint64_t bytes_total = decoder.bytes_received();
                const auto presented_age_ms = [probe_now](const auto time) -> std::int64_t {
                    if (time == std::chrono::steady_clock::time_point::min()) return -1;
                    return std::chrono::duration_cast<std::chrono::milliseconds>(probe_now - time).count();
                };
                SPDLOG_INFO("视频探针 udp_in=+{} custom_in=+{} udp_total={} custom_total={} udp_ago={}ms custom_ago={}ms loop={}/s | cbb pkt_in=+{} pkt_taken_total={} frames_out=+{} bytes_in=+{} displayed_only=1",
                            udp_frame_count - last_probe_udp_frames,
                            custom_frame_count - last_probe_custom_frames,
                            udp_frame_count,
                            custom_frame_count,
                            presented_age_ms(last_udp_frame_time),
                            presented_age_ms(last_custom_frame_time),
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

                for (std::size_t slot = 0; slot < peer_channels.size(); ++slot) {
                    const auto &channel = peer_channels[slot];
                    const auto link = channel->link ? channel->link->stats() : RoboGo::net::TeammateLinkStats{};
                    const auto video = channel->decoder.stats();
                    SPDLOG_INFO("队友探针 本机={} 队友={} 发={} 收={} 入解={} 解={} 错误={} {}x{}",
                                peer_identity, channel->robot_id, link.frames_sent, link.frames_received,
                                video.frames_submitted, video.frames_decoded, video.decode_errors, video.width, video.height);
                    if (commander_active) {
                        const auto texture = platform_backend.GetTextureView(kPeerTextureSlots[slot]);
                        SPDLOG_INFO("COMMANDER_VIDEO seat={} decoded={}x{} texture={}x{}", channel->robot_id, video.width, video.height,
                                    static_cast<int>(texture.size.x), static_cast<int>(texture.size.y));
                    }
                }
            }

            const TextureView udp_video_texture = platform_backend.GetTextureView(TextureSlot::UdpVideo);
            const TextureView custom_video_texture = platform_backend.GetTextureView(TextureSlot::CustomVideo);
            if (map_image && !platform_backend.GetTextureView(map_slot).IsReady())
                (void)platform_backend.UploadTexture(map_slot, *map_image);
            const TextureView mini_map_texture = platform_backend.GetTextureView(map_slot);

            // 联盟赛允许本轮已上传的新图直接参加这一轮绘制；成功 Present 后再更新探针。
            // 没有新图时，沿用每路既有的上一次显示提交时间与超时。
            const auto now = std::chrono::steady_clock::now();
            const bool udp_video_display_ready = IsVideoDisplayReady(
                    udp_video_texture,
                    last_udp_frame_time,
                    kUdpVideoFallbackTimeout,
                    now, alliance_signed_in && pending_video_presentation.udp);
            const bool custom_video_display_ready = IsVideoDisplayReady(
                    custom_video_texture,
                    last_custom_frame_time,
                    kCustomVideoLiveTimeout,
                    now, alliance_signed_in && pending_video_presentation.custom);
            const bool custom_video_hold_as_main = IsVideoDisplayReady(
                    custom_video_texture,
                    last_custom_frame_time,
                    kCustomVideoFallbackTimeout,
                    now);

            // 自动抢主画面只看「继续用」这一档（5 秒）；手动按 H 仍然按 1 秒那档给提示。
            const bool shortcuts_enabled = !commander_active && !map_owns_input && !teammate_overview &&
                                           (!alliance || (alliance_signed_in && !hud.menu_open));
            if (alliance) {
                if (shortcuts_enabled && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_H, false))
                    hud.industrial_main = !hud.industrial_main;
                prefer_custom_video_as_main = using_custom_video_as_main = hud.industrial_main;
            } else {
                UpdateVideoLayoutPreference(state, shortcuts_enabled, udp_video_display_ready, custom_video_display_ready,
                                            custom_video_hold_as_main, prefer_custom_video_as_main, using_custom_video_as_main);
            }
            if (shortcuts_enabled && ImGui::IsKeyPressed(ImGuiKey_I, false) && !ImGui::GetIO().WantTextInput)
                ToggleUdpVideoInversion(state, background_services);

            TeammateViewState teammate_view;
            teammate_view.count = static_cast<int>(peer_channels.size());
            teammate_view.collapsed = teammates_collapsed;
            teammate_view.take_main_screen = teammate_overview;
            teammate_view.focused = focused_peer;
            if (teammate_overview && peer_keys && (peer_channels.size() == 1 || focused_peer >= 0)) {
                const float wheel = ImGui::GetIO().MouseWheel;
                teammate_zoom = ImClamp(teammate_zoom + wheel * 0.25f, 1.0f, 4.0f);
            }
            teammate_view.zoom = teammate_zoom;
            for (std::size_t slot = 0; slot < peer_channels.size(); ++slot) {
                const auto &channel = peer_channels[slot];
                const auto texture = platform_backend.GetTextureView(kPeerTextureSlots[slot]);
                const auto link = channel->link ? channel->link->stats() : RoboGo::net::TeammateLinkStats{};
                auto &view = teammate_view.peers[slot];
                view.robot_id = channel->robot_id;
                view.label = channel->label;
                view.texture_ref = texture.texture_ref;
                view.texture_size = texture.size;
                view.paused = channel->paused;
                view.display_ready = !view.paused && IsVideoDisplayReady(texture, last_peer_frames[slot], kTeammateVideoOfflineTimeout,
                                                         now, pending_video_presentation.peers[slot]);
                view.live = !view.paused && IsVideoDisplayReady(texture, last_peer_frames[slot], kTeammateVideoLiveTimeout,
                                                now, pending_video_presentation.peers[slot]);
                view.peer_connected = link.peer_connected;
                view.peer_served = options.local_trial || link.peer_served;
                view.frame_inverted = link.peer_frame_inverted;
            }

            CommanderDeskActions commander_actions{};
            MainWindowActions ui_actions{};
            if (commander_active) {
                if (benchmark) {
                    commander_state.requested_mode = benchmark->Requested();
                    commander_state.show_markers = benchmark->MarkersEnabled();
                    commander_state.floating_cards = benchmark->FloatingCards();
                }
                const bool previously_paused = commander_state.replay.Blue3Paused();
                const auto replay_now = benchmark ? benchmark->ReplayTime(now) : now;
                commander_actions = ShowCommanderDesk(commander_state, teammate_view, state.game_status ? &*state.game_status : nullptr,
                                                       mini_map_texture.texture_ref.GetTexID(), replay_now,
                                                        benchmark ? benchmark->CameraSeconds() : -1.);
                if (previously_paused != commander_state.replay.Blue3Paused())
                    SPDLOG_INFO("指挥席本地样例 蓝3位置={}", commander_state.replay.Blue3Paused() ? "暂停" : "恢复");
                for (std::size_t slot = 0; slot < peer_channels.size(); ++slot) {
                    auto &decoder = peer_channels[slot]->decoder;
                    const auto source = decoder.stats();
                    decoder.SetPreviewWidth(commander_state.PreviewWidth(slot, source.width, source.height));
                }
                if (commander_actions.return_to_events) session_result = kReturnToLauncher;
                if (commander_actions.toggle_feed >= 0 && commander_actions.toggle_feed < teammate_view.count) {
                    const auto slot_index = static_cast<std::size_t>(commander_actions.toggle_feed);
                    background_services.ToggleReplayPeer(slot_index);
                    last_peer_frames[slot_index] = std::chrono::steady_clock::time_point::min();
                }
            } else {
                ui_actions = ShowMainWindow(
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
                    alliance ? custom_video_display_ready : custom_video_hold_as_main,
                    background_services.IsUdpVideoInverted(),
                    mini_map_texture.texture_ref,
                    mini_map_texture.size,
                    teammate_view, options.local_trial, hud.menu_open, map_owns_input,
                    cup_replay ? &*cup_replay : nullptr, cup_replay ? &teammate_view : nullptr);
                if (cup_replay && ui_actions.toggle_replay_feed >= 0 &&
                    ui_actions.toggle_replay_feed < teammate_view.count) {
                    const auto slot = static_cast<size_t>(ui_actions.toggle_replay_feed);
                    background_services.ToggleReplayPeer(slot);
                    last_peer_frames[slot] = Clock::time_point::min();
                }
                // The comparison command selects the same local seat as the start button.
                if (benchmark) ui_actions.commander_team = 0;
                if (commander_allowed && ui_actions.commander_team >= 0) {
                    commander_state.team = ui_actions.commander_team;
                    commander_state.replay.Start(options.commander_comparison == CommanderComparison::Layouts
                                                ? Clock::time_point{} : Clock::now());
                    background_services.SelectCommanderPeers(options, commander_state.team);
                    RoboGo::arena::ResetCamera();
                    trial_start = Clock::now();
                    trial_second = -1;
                    commander_active = true;
                }
            }
            if (ui_actions.return_to_events) session_result = kReturnToLauncher;
            if (ui_actions.close_menu) hud.menu_open = false;
            if (ui_actions.open_menu) hud.menu_open = true;
            platform_backend.SetMatchDisplayShortcuts(!commander_active && connected_identity != 0 &&
                                                      (alliance ? !hud.menu_open : is_in_game_mode()),
                                                      alliance || (cup_seat && !is_marker_mode()));
            if (ui_actions.toggle_main_video) hud.industrial_main = !hud.industrial_main;
            if (ui_actions.toggle_udp_video_inversion) {
                ToggleUdpVideoInversion(state, background_services);
            }
            if (teammate_overview && ui_actions.teammate_slot_clicked >= 0 &&
                ui_actions.teammate_slot_clicked < teammate_view.count) {
                focused_peer = focused_peer < 0 ? ui_actions.teammate_slot_clicked : -1;
                teammate_zoom = 1.0f;
            }

            if (!alliance && !commander_active) {
                if (peer_input_step.sample == KeyboardMouseSample::Match) synchronize_game_input_capture();
            } else if (is_in_game_mode() || is_marker_mode()) {
                reset_client_session_input();
            }

            ImGui::Render();
            const auto render_begin = benchmark ? Clock::now() : Clock::time_point{};
            const bool presented = platform_backend.RenderFrame(ImGui::GetDrawData());
            if (presented) {
                CommitVideoPresentation(pending_video_presentation,
                                        last_udp_frame_time, last_custom_frame_time, last_peer_frames,
                                        udp_frame_count, custom_frame_count);
            }
            if (cup_benchmark) {
                std::array<int, 2> seats{};
                for (std::size_t i = 0; i < seats.size() && i < peer_channels.size(); ++i)
                    seats[i] = peer_channels[i]->robot_id;
                const auto size = ImGui::GetIO().DisplaySize;
                const bool expected_view = connected_identity == 103 && !menu_open &&
                    (cup_benchmark->Map() ? hud.map_open : teammate_overview && !map_owns_input);
                cup_benchmark->Observe(platform_backend.IsFullscreen(), presented, static_cast<int>(size.x),
                    static_cast<int>(size.y), ui_actions.map_drawn_mask, pending_video_presentation.peers, expected_view, seats);
                if (cup_benchmark->Done()) {
                    if (!cup_benchmark->Passed()) session_result = 1;
                    break;
                }
            }
            if (benchmark && commander_active) {
                const auto rendered = Clock::now();
                if (presented && rendered - last_present > std::chrono::milliseconds(33) &&
                    rendered - previous_slow_log >= std::chrono::seconds(1)) {
                    auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
                    SPDLOG_INFO("COMMANDER_SLOW wait_and_events_ms={:.2f} upload_ms={:.2f} ui_ms={:.2f} render_present_ms={:.2f}",
                        ms(upload_begin-last_present), ms(upload_end-upload_begin), ms(render_begin-upload_end), ms(rendered-render_begin));
                    previous_slow_log = rendered;
                }
                if (presented) last_present = rendered;
                unsigned fresh = 0;
                for (unsigned i = 0; i < pending_video_presentation.peers.size(); ++i)
                    if (pending_video_presentation.peers[i]) fresh |= 1u << i;
                const auto size = ImGui::GetIO().DisplaySize;
                benchmark->Observe(platform_backend.IsFullscreen(), presented, commander_actions.actual_mode,
                                   commander_actions.drawn_mask, fresh, static_cast<int>(size.x), static_cast<int>(size.y),
                                   commander_actions.map_marker_count);
                if (benchmark->Done()) break;
            }
            if (session_result == kReturnToLauncher) break;
        }

        SPDLOG_INFO("开始关闭网络与后台线程");
        background_services.Stop();
        background_services.DrainCustomVideoLogs(state);
        SPDLOG_INFO("开始执行图形资源清理");
        platform_backend.Shutdown();
        SPDLOG_INFO("程序退出完成");
    }

    set_game_input_capture_handler({});
    reset_client_session_input();
    return session_result;
}

}// namespace RoboGo::app
