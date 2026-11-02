#include "teammate_link.h"

#include "logging/logging.h"

namespace RoboGo::net {

namespace {

constexpr std::uint32_t kHandshakeMagic = 0x524D544Du;// 'R''M''T''M'
constexpr std::uint8_t kHandshakeVersion = 1;
constexpr std::uint8_t kFlagInverted = 0x01u;
constexpr std::size_t kHandshakeSize = 8;
// 单帧上限。正常 1080p 关键帧也就几十 KB，留到 8 MB 只是防坏包把内存吃光。
constexpr std::uint32_t kMaxPayloadBytes = 8u * 1024u * 1024u;
constexpr std::size_t kMaxParameterSetBytes = 64u * 1024u;
// 阻塞读写的兜底超时。有它才能在退出时干净 join；超时就当链路异常，断开重连。
constexpr int kSocketTimeoutMs = 2000;
constexpr auto kSendPopTimeout = std::chrono::milliseconds(100);

using Bytes = std::vector<std::uint8_t>;
using TcpSocket = boost::asio::ip::tcp::socket;

void WriteU32BE(std::uint8_t *dest, const std::uint32_t value) {
    dest[0] = static_cast<std::uint8_t>(value >> 24);
    dest[1] = static_cast<std::uint8_t>(value >> 16);
    dest[2] = static_cast<std::uint8_t>(value >> 8);
    dest[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t ReadU32BE(const std::uint8_t *src) {
    return (static_cast<std::uint32_t>(src[0]) << 24)
           | (static_cast<std::uint32_t>(src[1]) << 16)
           | (static_cast<std::uint32_t>(src[2]) << 8)
           | static_cast<std::uint32_t>(src[3]);
}

// Windows 的 SO_RCVTIMEO 要的是 DWORD 毫秒，Linux 要的是 timeval，分开写。
void ApplySocketTimeouts(TcpSocket &socket, const int timeout_ms) {
    const auto native = socket.native_handle();
#ifdef _WIN32
    const DWORD timeout = static_cast<DWORD>(timeout_ms);
    setsockopt(native, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    setsockopt(native, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#else
    const timeval timeout{static_cast<time_t>(timeout_ms / 1000),
                          static_cast<suseconds_t>((timeout_ms % 1000) * 1000)};
    setsockopt(native, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(native, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

bool ReadExact(TcpSocket &socket, std::uint8_t *data, const std::size_t size) {
    boost::system::error_code error;
    boost::asio::read(socket, boost::asio::buffer(data, size), error);
    return !error;
}

bool WriteAll(TcpSocket &socket, const std::uint8_t *data, const std::size_t size) {
    boost::system::error_code error;
    boost::asio::write(socket, boost::asio::buffer(data, size), error);
    return !error;
}

bool WriteFrame(TcpSocket &socket, const Bytes &payload, const std::uint32_t flags) {
    // 每帧都带 flags：这样对方按 I 切换翻转后，下一次握手才生效的问题就不存在了。
    std::uint8_t header[8];
    WriteU32BE(header, static_cast<std::uint32_t>(payload.size()));
    WriteU32BE(header + 4, flags);
    if (!WriteAll(socket, header, sizeof(header))) {
        return false;
    }
    return payload.empty() || WriteAll(socket, payload.data(), payload.size());
}

int NalTypeAt(const Bytes &buffer, const std::size_t start_code_bytes) {
    if (start_code_bytes >= buffer.size()) {
        return -1;
    }
    return (buffer[start_code_bytes] >> 1) & 0x3F;
}

int CachedNalType(const Bytes &parameter_set) {
    if (parameter_set.size() >= 4 && parameter_set[2] == 0 && parameter_set[3] == 1) {
        return NalTypeAt(parameter_set, 4);
    }
    return NalTypeAt(parameter_set, 3);
}

// 扫描 Annex B 码流里所有的 VPS/SPS/PPS（HEVC NAL 32/33/34），整段缓存下来。
// 晚连上来的一方没有这些参数集就解不出画面，所以每次连接建立时要把它们先补发一遍。
void CollectParameterSets(const Bytes &frame, std::vector<Bytes> &parameter_sets) {
    const std::size_t size = frame.size();
    std::size_t position = 0;

    while (position + 3 <= size) {
        std::size_t start_code_bytes = 0;
        if (position + 4 <= size && frame[position] == 0 && frame[position + 1] == 0
            && frame[position + 2] == 0 && frame[position + 3] == 1) {
            start_code_bytes = 4;
        } else if (frame[position] == 0 && frame[position + 1] == 0 && frame[position + 2] == 1) {
            start_code_bytes = 3;
        } else {
            ++position;
            continue;
        }

        const std::size_t nal_start = position;
        const int nal_type = NalTypeAt(frame, position + start_code_bytes);

        // 找下一个起始码作为本 NAL 的结尾
        std::size_t next = position + start_code_bytes + 1;
        while (next + 3 <= size) {
            if (frame[next] == 0 && frame[next + 1] == 0
                && (frame[next + 2] == 1
                    || (next + 3 < size && frame[next + 2] == 0 && frame[next + 3] == 1))) {
                break;
            }
            ++next;
        }
        if (next + 3 > size) {
            next = size;
        }

        if ((nal_type == 32 || nal_type == 33 || nal_type == 34) && next > nal_start) {
            const std::size_t nal_bytes = next - nal_start;
            if (nal_bytes <= kMaxParameterSetBytes) {
                // 同一类型的参数集只留最新的那一份
                std::erase_if(parameter_sets, [nal_type](const Bytes &existing) {
                    return CachedNalType(existing) == nal_type;
                });
                parameter_sets.emplace_back(frame.begin() + static_cast<std::ptrdiff_t>(nal_start),
                                            frame.begin() + static_cast<std::ptrdiff_t>(next));
            }
        }
        position = next;
    }
}

bool ConnectWithTimeout(boost::asio::io_context &io_context,
                        TcpSocket &socket,
                        const boost::asio::ip::tcp::endpoint &endpoint,
                        const std::chrono::milliseconds timeout) {
    boost::asio::steady_timer timer(io_context);
    bool connected = false;
    socket.async_connect(endpoint, [&](const boost::system::error_code &error) {
        connected = !error;
        // Boost 1.91 的 waitable_timer 只有 cancel() / cancel_one()，没有带 error_code 的重载，
        // 失败会抛 system_error。取消失败不影响任何判断，吞掉即可。
        try {
            timer.cancel();
        } catch (const boost::system::system_error &) {
        }
    });
    timer.expires_after(timeout);
    timer.async_wait([&](const boost::system::error_code &error) {
        if (!error) {
            // 超时：取消在途的 connect。要显式 cancel，不能只 close ——
            // Windows 上 close() 不一定会让挂起的 connect 立刻回调，
            // 可能要等 TCP 自己的 SYN 重传超时（十几秒），重连循环就被拖住了。
            boost::system::error_code ignored;
            socket.cancel(ignored);
            socket.close(ignored);
        }
    });
    io_context.restart();
    io_context.run();
    if (!connected) {
        boost::system::error_code ignored;
        socket.close(ignored);
    }
    return connected;
}

}// namespace

struct TeammateLink::Shared {
    TeammateLinkConfig config;
    FrameHandler on_frame;

    // 发送目标单独一份，允许主循环热改（切角色）。受 peer_mutex 保护。
    // config.peer 只在构造时写一次，发送线程不再读它。
    mutable std::mutex peer_mutex;
    TeammateEndpoint peer;
    // 每改一次 +1；发送线程发现号变了就断开当前连接、用新地址重连。
    std::atomic<std::uint64_t> peer_generation{0};

    std::atomic<bool> running{true};
    std::atomic<bool> local_inverted{false};

    // 发送队列：主循环入队，发送线程出队
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<Bytes> send_queue;

    // 服务端用来接收连接的 acceptor。
    // 用 shared_ptr：接收线程在 accept() 阻塞时会长期持有它，stop() 从另一个线程 close()
    // 也不会和析构撞车（否则就是 use-after-free，加了锁又会变成死锁）。
    std::mutex acceptor_mutex;
    std::shared_ptr<boost::asio::ip::tcp::acceptor> acceptor;

    std::atomic<std::uint64_t> frames_queued{0};
    std::atomic<std::uint64_t> frames_sent{0};
    std::atomic<std::uint64_t> frames_dropped{0};
    std::atomic<std::uint64_t> bytes_sent{0};
    std::atomic<std::uint64_t> frames_received{0};
    std::atomic<std::uint64_t> bytes_received{0};
    std::atomic<bool> peer_connected{false};
    std::atomic<bool> peer_served{false};
    std::atomic<bool> peer_frame_inverted{false};

    mutable std::mutex peer_text_mutex;
    std::string peer_address;
};

TeammateLink::TeammateLink(TeammateLinkConfig config, FrameHandler on_frame)
    : shared_(std::make_shared<Shared>()) {
    shared_->config = std::move(config);
    shared_->on_frame = std::move(on_frame);
    shared_->peer = shared_->config.peer;

    // 接收线程总是启动：listen_port 为 0 时 run_receiver 自己会退出。
    // 端口被占用（比如本机开了第二个实例）只影响「收」，不影响「发」——
    // 默认启用后这条链路不能因为 bind 失败就整个不工作。
    receiver_thread_ = std::thread([this]() { run_receiver(); });
    if (!shared_->config.peer.host.empty() && shared_->config.peer.port != 0) {
        try {
            sender_thread_ = std::thread([this]() { run_sender(); });
        } catch (const std::exception &error) {
            // 极端情况（线程数耗尽）。绝不能把整个客户端带走：这条链路降级成只收不发。
            SPDLOG_ERROR("队友链路：发送线程创建失败（{}），只收不发", error.what());
        }
    }
    SPDLOG_INFO("队友链路已创建 peer={}:{} listen_port={}",
                shared_->config.peer.host,
                shared_->config.peer.port,
                shared_->config.listen_port);
}

TeammateLink::~TeammateLink() {
    stop();
}

void TeammateLink::push_frame(const Bytes &frame) {
    if (!shared_->running.load(std::memory_order_relaxed) || frame.empty()) {
        return;
    }
    {
        std::lock_guard lock(shared_->queue_mutex);
        // 队列满 → 丢最旧的一帧。绝不能为了队友让主链路卡住。
        while (shared_->send_queue.size() >= shared_->config.send_queue_capacity) {
            shared_->send_queue.pop_front();
            shared_->frames_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        shared_->send_queue.push_back(frame);
        shared_->frames_queued.fetch_add(1, std::memory_order_relaxed);
    }
    shared_->queue_cv.notify_one();
}

void TeammateLink::set_local_inverted(const bool inverted) {
    shared_->local_inverted.store(inverted, std::memory_order_relaxed);
}

void TeammateLink::set_peer(TeammateEndpoint peer) {
    const std::string text = peer.host + ":" + std::to_string(peer.port);
    {
        std::lock_guard lock(shared_->peer_mutex);
        shared_->peer = std::move(peer);
        // 发送线程看见这个号变了就主动断开重连，不用等这次写失败。
        shared_->peer_generation.fetch_add(1, std::memory_order_relaxed);
    }
    // 队列里还没发出去的帧是给上一个角色的，清掉。
    {
        std::lock_guard lock(shared_->queue_mutex);
        shared_->frames_dropped.fetch_add(shared_->send_queue.size(), std::memory_order_relaxed);
        shared_->send_queue.clear();
    }
    shared_->queue_cv.notify_all();
    SPDLOG_INFO("队友链路：发送目标已改为 {}", text);
}

TeammateLinkStats TeammateLink::stats() const {
    TeammateLinkStats result;
    result.frames_queued = shared_->frames_queued.load(std::memory_order_relaxed);
    result.frames_sent = shared_->frames_sent.load(std::memory_order_relaxed);
    result.frames_dropped = shared_->frames_dropped.load(std::memory_order_relaxed);
    result.bytes_sent = shared_->bytes_sent.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(shared_->queue_mutex);
        result.queued_now = shared_->send_queue.size();
    }
    result.peer_connected = shared_->peer_connected.load(std::memory_order_relaxed);
    result.frames_received = shared_->frames_received.load(std::memory_order_relaxed);
    result.bytes_received = shared_->bytes_received.load(std::memory_order_relaxed);
    result.peer_served = shared_->peer_served.load(std::memory_order_relaxed);
    result.peer_frame_inverted = shared_->peer_frame_inverted.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(shared_->peer_text_mutex);
        result.peer_address = shared_->peer_address;
    }
    return result;
}

void TeammateLink::stop() {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    shared_->running.store(false, std::memory_order_relaxed);
    shared_->queue_cv.notify_all();
    {
        std::lock_guard lock(shared_->acceptor_mutex);
        if (shared_->acceptor) {
            boost::system::error_code ignored;
            shared_->acceptor->close(ignored);
        }
    }
    if (sender_thread_.joinable()) {
        sender_thread_.join();
    }
    if (receiver_thread_.joinable()) {
        receiver_thread_.join();
    }
    SPDLOG_INFO("队友链路已停止");
}

bool TeammateLink::ParseEndpoint(const std::string &text, TeammateEndpoint &endpoint, std::string &error_message) {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
        error_message = "格式应为 主机:端口，例如 10.77.0.2:47700";
        return false;
    }
    const std::string port_text = text.substr(colon + 1);
    try {
        std::size_t parsed = 0;
        const unsigned long port = std::stoul(port_text, &parsed, 10);
        if (parsed != port_text.size() || port == 0 || port > 65535) {
            error_message = "端口必须是 1~65535";
            return false;
        }
        endpoint.host = text.substr(0, colon);
        endpoint.port = static_cast<std::uint16_t>(port);
        return true;
    } catch (const std::exception &) {
        error_message = "端口不是合法数字";
        return false;
    }
}

void TeammateLink::run_sender() {
    // 注意是类型别名（using），不是 namespace 别名。
    // MSVC 不接受在函数作用域里给 boost::asio::ip::tcp 起 namespace 别名（C2879）。
    using tcp = boost::asio::ip::tcp;
    boost::asio::io_context io_context;
    std::vector<Bytes> parameter_sets;
    std::uint64_t connect_failures = 0;

    while (shared_->running.load(std::memory_order_relaxed)) {
        // 每轮重连都重新读一次目标地址：标题条切角色时主循环会改它。
        TeammateEndpoint peer;
        std::uint64_t peer_generation = 0;
        {
            std::lock_guard lock(shared_->peer_mutex);
            peer = shared_->peer;
            peer_generation = shared_->peer_generation.load(std::memory_order_relaxed);
        }
        boost::system::error_code address_error;
        const auto address = boost::asio::ip::make_address(peer.host, address_error);
        if (address_error) {
            // 地址不合法（ini 里写错）不能退出线程：标题条切一下角色就该能恢复。
            // 同一个坏地址只报一次，免得每秒刷屏。
            static thread_local std::uint64_t last_bad_generation = ~std::uint64_t{0};
            if (last_bad_generation != peer_generation) {
                last_bad_generation = peer_generation;
                SPDLOG_ERROR("队友链路：对方地址不合法 host={}（切一下角色或改 ini 即可恢复）", peer.host);
            }
            std::this_thread::sleep_for(shared_->config.reconnect_delay);
            continue;
        }
        const auto endpoint = tcp::endpoint(address, peer.port);

        TcpSocket socket(io_context);
        if (!ConnectWithTimeout(io_context, socket, endpoint, shared_->config.connect_timeout)) {
            // 没插队友网线是常态（单机调试、平时单人用）：每秒都刷 WARN 会把日志淹掉，
            // 第一次打，之后每 60 次（≈1 分钟）提醒一次。
            ++connect_failures;
            if (connect_failures == 1 || connect_failures % 60 == 0) {
                SPDLOG_WARN("队友链路：连接 {}:{} 失败（已连续 {} 次），{}ms 后重试",
                            peer.host,
                            peer.port,
                            connect_failures,
                            shared_->config.reconnect_delay.count());
            }
            std::this_thread::sleep_for(shared_->config.reconnect_delay);
            continue;
        }
        connect_failures = 0;

        boost::system::error_code option_error;
        socket.set_option(tcp::no_delay(true), option_error);
        ApplySocketTimeouts(socket, kSocketTimeoutMs);
        SPDLOG_INFO("队友链路：已连接 {}:{}", peer.host, peer.port);
        shared_->peer_connected.store(true, std::memory_order_relaxed);

        // 握手：告诉对方我这边画面是不是倒的（仅作初始值，每帧还会再带一遍）
        std::uint8_t handshake[kHandshakeSize] = {};
        WriteU32BE(handshake, kHandshakeMagic);
        handshake[4] = kHandshakeVersion;
        handshake[5] = shared_->local_inverted.load(std::memory_order_relaxed) ? kFlagInverted : 0;
        bool link_ok = WriteAll(socket, handshake, sizeof(handshake));

        // 把缓存的参数集补发一遍，让刚启动的对方能立刻解码
        if (link_ok) {
            const std::uint32_t parameter_flags =
                    shared_->local_inverted.load(std::memory_order_relaxed) ? kFlagInverted : 0;
            for (const auto &parameter_set: parameter_sets) {
                if (!WriteFrame(socket, parameter_set, parameter_flags)) {
                    link_ok = false;
                    break;
                }
            }
            if (!parameter_sets.empty()) {
                SPDLOG_INFO("队友链路：已补发 {} 组参数集", parameter_sets.size());
            }
        }

        while (link_ok && shared_->running.load(std::memory_order_relaxed)) {
            // 角色切换：立刻断开这条连接，外层循环会拿新地址重连。
            if (shared_->peer_generation.load(std::memory_order_relaxed) != peer_generation) {
                SPDLOG_INFO("队友链路：发送目标已切换，断开旧连接准备重连");
                break;
            }
            Bytes frame;
            {
                std::unique_lock lock(shared_->queue_mutex);
                shared_->queue_cv.wait_for(lock, kSendPopTimeout, [this]() {
                    return !shared_->send_queue.empty()
                           || !shared_->running.load(std::memory_order_relaxed);
                });
                if (shared_->send_queue.empty()) {
                    continue;
                }
                frame = std::move(shared_->send_queue.front());
                shared_->send_queue.pop_front();
            }

            // 顺手把参数集缓存下来，供以后新连上来的对方使用
            CollectParameterSets(frame, parameter_sets);

            const std::uint32_t frame_flags =
                    shared_->local_inverted.load(std::memory_order_relaxed) ? kFlagInverted : 0;
            if (!WriteFrame(socket, frame, frame_flags)) {
                SPDLOG_WARN("队友链路：发送失败，准备重连");
                link_ok = false;
                break;
            }
            shared_->frames_sent.fetch_add(1, std::memory_order_relaxed);
            shared_->bytes_sent.fetch_add(frame.size(), std::memory_order_relaxed);
        }

        shared_->peer_connected.store(false, std::memory_order_relaxed);
        boost::system::error_code ignored;
        socket.shutdown(tcp::socket::shutdown_both, ignored);
        socket.close(ignored);
        if (shared_->running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(shared_->config.reconnect_delay);
        }
    }
    SPDLOG_INFO("队友链路：发送线程已退出");
}

void TeammateLink::run_receiver() {
    using tcp = boost::asio::ip::tcp;
    if (shared_->config.listen_port == 0) {
        SPDLOG_INFO("队友链路：未配置监听端口，只发不收");
        return;
    }
    boost::asio::io_context io_context;

    // 默认启用后，47700 被占（本机第二个实例、别的客户端）是现场常态。
    // 逐级往上试 10 个端口，先 bind 上手最重要；实在占不到就每 5 秒重试一次。
    constexpr std::uint16_t kMaxPortProbes = 10;
    std::shared_ptr<tcp::acceptor> acceptor;
    std::uint16_t bound_port = 0;
    while (shared_->running.load(std::memory_order_relaxed)) {
        for (std::uint16_t offset = 0; offset < kMaxPortProbes; ++offset) {
            if (!shared_->running.load(std::memory_order_relaxed)) {
                break;
            }
            const auto candidate = static_cast<std::uint16_t>(shared_->config.listen_port + offset);
            if (candidate < shared_->config.listen_port) {
                // 往上试会越过 65535 绕回小端口（甚至绕到 0 = 让系统随便挑），没意义，直接判失败。
                break;
            }
            auto attempt = std::make_shared<tcp::acceptor>(io_context);
            boost::system::error_code error;
            attempt->open(tcp::v4(), error);
            if (!error) {
                attempt->set_option(boost::asio::socket_base::reuse_address(true), error);
            }
            if (!error) {
                attempt->bind(tcp::endpoint(tcp::v4(), candidate), error);
            }
            if (!error) {
                attempt->listen(boost::asio::socket_base::max_listen_connections, error);
            }
            if (!error) {
                acceptor = std::move(attempt);
                bound_port = candidate;
                break;
            }
            if (offset == 0) {
                SPDLOG_WARN("队友链路：监听端口 {} 被占用（{}），自动往上换一个",
                            candidate,
                            error.message());
            }
        }
        if (acceptor != nullptr) {
            break;
        }
        SPDLOG_WARN("队友链路：{}~{} 端口全被占用，5 秒后重试（只影响收画面）",
                    shared_->config.listen_port,
                    static_cast<unsigned>(shared_->config.listen_port)
                            + static_cast<unsigned>(kMaxPortProbes) - 1u);
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    if (acceptor == nullptr) {
        return;
    }
    if (bound_port != shared_->config.listen_port) {
        SPDLOG_WARN("队友链路：实际监听端口是 {}（不是 {}）——队友要填这个端口才能连进来",
                    bound_port,
                    shared_->config.listen_port);
    }
    boost::system::error_code error;
    // 非阻塞 + 轮询：这样 stop() 从别的线程 close() 之后，这里下一轮就能退出去。
    // （Linux 上 close() 并不会唤醒另一个线程里阻塞中的 accept()，会卡死 join。）
    acceptor->non_blocking(true, error);
    SPDLOG_INFO("队友链路：正在监听端口 {}", bound_port);
    {
        std::lock_guard lock(shared_->acceptor_mutex);
        shared_->acceptor = acceptor;
    }

    while (shared_->running.load(std::memory_order_relaxed)) {
        TcpSocket socket(io_context);
        boost::system::error_code accept_error;
        // 本地多持一份 shared_ptr：stop() 关掉 acceptor 后我们这里还安全。
        acceptor->accept(socket, accept_error);
        if (accept_error) {
            if (shared_->running.load(std::memory_order_relaxed)) {
                // 没人连进来是常态，不要刷日志
                if (accept_error != boost::asio::error::would_block) {
                    SPDLOG_WARN("队友链路：accept 失败 message={}", accept_error.message());
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                }
            }
            continue;
        }

        boost::system::error_code option_error;
        // acceptor 是非阻塞的，新 socket 可能继承这个属性，明确改回阻塞再走超时读写。
        socket.non_blocking(false, option_error);
        socket.set_option(tcp::no_delay(true), option_error);
        ApplySocketTimeouts(socket, kSocketTimeoutMs);

        std::string peer_text = "unknown";
        try {
            const auto remote = socket.remote_endpoint();
            peer_text = remote.address().to_string() + ":" + std::to_string(remote.port());
        } catch (const std::exception &) {
        }
        {
            std::lock_guard lock(shared_->peer_text_mutex);
            shared_->peer_address = peer_text;
        }
        shared_->peer_served.store(true, std::memory_order_relaxed);
        SPDLOG_INFO("队友链路：对方已连接 {}", peer_text);

        // 握手
        std::uint8_t handshake[kHandshakeSize] = {};
        if (ReadExact(socket, handshake, sizeof(handshake))) {
            if (ReadU32BE(handshake) != kHandshakeMagic) {
                SPDLOG_WARN("队友链路：握手魔数不匹配，断开 {}", peer_text);
                boost::system::error_code ignored;
                socket.close(ignored);
                continue;
            }
            if (handshake[4] != kHandshakeVersion) {
                SPDLOG_WARN("队友链路：协议版本不一致 local={} peer={}（两端客户端需要同一版本）",
                            kHandshakeVersion,
                            handshake[4]);
            }
            shared_->peer_frame_inverted.store((handshake[5] & kFlagInverted) != 0,
                                               std::memory_order_relaxed);
            SPDLOG_INFO("队友链路：握手成功 版本={} 对方画面倒置={}",
                        handshake[4],
                        (handshake[5] & kFlagInverted) != 0 ? "是" : "否");
        } else {
            SPDLOG_WARN("队友链路：握手读取失败 {}", peer_text);
            boost::system::error_code ignored;
            socket.close(ignored);
            continue;
        }

        std::uint8_t header[8];
        while (shared_->running.load(std::memory_order_relaxed)) {
            if (!ReadExact(socket, header, sizeof(header))) {
                break;
            }
            const std::uint32_t payload_bytes = ReadU32BE(header);
            const std::uint32_t frame_flags = ReadU32BE(header + 4);
            // 每帧都刷新朝向：对方按 I 之后画面立刻跟着翻，不用重连。
            shared_->peer_frame_inverted.store((frame_flags & kFlagInverted) != 0,
                                               std::memory_order_relaxed);
            if (payload_bytes == 0 || payload_bytes > kMaxPayloadBytes) {
                SPDLOG_WARN("队友链路：非法的负载长度 {}，断开 {}", payload_bytes, peer_text);
                break;
            }
            Bytes payload(payload_bytes);
            if (!ReadExact(socket, payload.data(), payload.size())) {
                break;
            }
            shared_->frames_received.fetch_add(1, std::memory_order_relaxed);
            shared_->bytes_received.fetch_add(payload.size(), std::memory_order_relaxed);
            // 解码由上层在自己的线程里做，这里只做一次搬运
            if (shared_->on_frame) {
                shared_->on_frame(std::move(payload));
            }
        }

        SPDLOG_INFO("队友链路：对方断开 {}", peer_text);
        boost::system::error_code ignored;
        socket.shutdown(tcp::socket::shutdown_both, ignored);
        socket.close(ignored);
    }
    SPDLOG_INFO("队友链路：接收线程已退出");
}

}// namespace RoboGo::net
