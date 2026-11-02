#pragma once

#include <pch.h>

// 队友画面链路（2027 赛季自定义客户端互联）：
//   两台电脑各挂一条独立的单向 TCP，互相把「官方 UDP 图传的原始 HEVC 字节」原样转发。
//   不动字节 → 画质与官方图传完全一致，也省掉一次编码。
//
// 线格式（大端）：
//   [握手 8 字节]  magic 'R' 'M' 'T' 'M' + version(1) + flags(1) + reserved(2)
//                  flags bit0 = 发送端画面是倒的（和 UDP 那一路共用同一个翻转开关）
//   [帧]   payload_bytes(4) + flags(4) + payload
//                  每帧都带 flags，所以任一方按 I 切换翻转，画面立刻跟着翻，不用重连。
//   晚连上来的一方缺 VPS/SPS/PPS，所以服务端在握手后先把缓存的参数集补发一遍。
//
// 线程模型：
//   主循环每帧调 push_frame()，只做一次队列入队（满了丢最旧），绝不阻塞。
//   网络收发、参数集抽取、补发参数集都在本类自己的线程里，不占 io_context。
namespace RoboGo::net {

struct TeammateEndpoint {
    std::string host;
    std::uint16_t port = 0;
};

struct TeammateLinkConfig {
    // 对方地址。host 为空表示不发送（只收）。
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

    // 主循环调用：更新「我方画面是否倒置」，会随下一次握手告诉对方。
    void set_local_inverted(bool inverted);

    // 主循环调用：换发送目标（标题条上切 A/B 角色时用）。
    // 只改 peer，不动监听端口——监听 socket 只在启动时 bind 一次，热改要整条链路重建。
    // 发送线程下一轮重连就用新地址；旧的待发帧会清掉，避免把画面发给刚换下去的那台。
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
