#pragma once

#include "app/application_types.h"

// 同队视频专线。每对登录岗位各有独立收发线程和有界队列。接收回调在网络线程里运行。
namespace RoboGo::net {

struct TeammateEndpoint {
    std::string host;
    std::uint16_t port = 0;
};

struct TeamSeatPair {
    app::Competition competition;
    int local_robot_id;
    int remote_robot_id;
    bool loopback;
};

struct TeammateLinkConfig {
    std::optional<TeamSeatPair> seats;
    // 旧手工模式用 host:port；按岗位配对时自动寻找专线地址。
    TeammateEndpoint peer;
    // 本地监听端口。0 表示不接收。
    std::uint16_t listen_port = 0;
    // 发送队列深度（帧）。满了丢最旧的。
    std::size_t send_queue_capacity = 8;
    // 断线重连间隔。
    std::chrono::milliseconds reconnect_delay{1000};
    // 建立连接的超时。
    std::chrono::milliseconds connect_timeout{2000};
};

std::vector<int> TeamSeatIds(app::Competition competition, int local_robot_id);
const char *TeamSeatName(app::Competition competition, int robot_id);
TeammateLinkConfig TeamSeatLinkConfig(app::Competition competition, int local_robot_id,
                                      int remote_robot_id, bool loopback = false);

struct TeammateLinkStats {
    // 发送侧
    std::uint64_t frames_queued = 0;// 交给队列的帧
    std::uint64_t frames_sent = 0;  // 真正写进 socket 的帧
    std::uint64_t frames_dropped = 0;// 队列满丢掉的帧
    std::uint64_t bytes_sent = 0;
    std::uint64_t queued_now = 0;
    bool peer_connected = false;
    // 接收侧
    std::uint64_t frames_received = 0;
    std::uint64_t bytes_received = 0;
    bool peer_served = false;// 有人连到我们
    bool peer_frame_inverted = false;
    std::string peer_address;
};

class TeammateLink {
public:
    // on_frame: 收到对方的原始码流（在接收线程里调用，不要做重活）
    using FrameHandler = std::function<void(std::vector<std::uint8_t> &&frame)>;

    TeammateLink(TeammateLinkConfig config, FrameHandler on_frame);
    ~TeammateLink();

    TeammateLink(const TeammateLink &) = delete;
    TeammateLink &operator=(const TeammateLink &) = delete;
    TeammateLink(TeammateLink &&) = delete;
    TeammateLink &operator=(TeammateLink &&) = delete;

    // 主循环调用：把一帧原始码流放进发送队列。队列满时丢最旧的那帧。
    void push_frame(const std::vector<std::uint8_t> &frame);

    // 方向随每个视频包发送。
    void set_local_inverted(bool inverted);

    // 旧手工地址诊断模式可切换对端。已登录岗位固定在相应席位。
    void set_peer(TeammateEndpoint peer);

    // 主循环调用：只读统计。
    [[nodiscard]] TeammateLinkStats stats() const;

    void stop();

    // 解析 "host:port"；失败返回 false 并写 error_message。
    static bool ParseEndpoint(const std::string &text, TeammateEndpoint &endpoint, std::string &error_message);

private:
    struct Shared;

    std::shared_ptr<Shared> shared_;
    std::thread sender_thread_;
    std::thread receiver_thread_;
    bool stopped_ = false;

    void run_sender();
    void run_receiver();
};

}// namespace RoboGo::net
