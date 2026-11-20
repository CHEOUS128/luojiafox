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

int AnnexBPrefix(const Bytes &frame, std::size_t at) {
    if (at + 3 > frame.size() || frame[at] || frame[at+1]) return 0;
    if (frame[at+2] == 1) return 3;
    return at + 4 <= frame.size() && !frame[at+2] && frame[at+3] == 1 ? 4 : 0;
}

using ParameterSets = std::array<Bytes, 3>;

// Camera headers precede the first coded slice. The common P/IDR packet
// returns after inspecting its first byte. Only metadata NALs need scanning.
void CacheHeaders(const Bytes &frame, ParameterSets &saved) {
    std::size_t at = 0;
    while (at < frame.size()) {
        const int prefix = AnnexBPrefix(frame, at);
        if (!prefix || at + prefix >= frame.size()) return;
        const auto type = (frame[at+prefix] >> 1) & 63;
        if (type < 32) return;
        std::size_t next = at + prefix + 1;
        while (next < frame.size() && !AnnexBPrefix(frame, next)) ++next;
        if (type >= 32 && type <= 34 && next - at <= kMaxParameterSetBytes) {
            saved[type-32].assign(frame.begin() + at, frame.begin() + next);
        }
        at = next;
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

constexpr std::array<int, 3> kSeats{1, 3, 4};
constexpr std::array<const char *, 3> kTeamHosts{"10.77.0.1", "10.77.0.2", "10.77.0.3"};
constexpr std::uint8_t kSeatVersion = 2;
using tcp = boost::asio::ip::tcp;
using Error = boost::system::error_code;
using Handshake = std::array<std::uint8_t, kHandshakeSize>;

int CompetitionCode(app::Competition competition) {
    switch (competition) {
        case app::Competition::Uc2026: return 0;
        case app::Competition::Ul2027: return 1;
        case app::Competition::Uc2027: return 2;
    }
    return -1;
}

int SeatIndex(int id) {
    const int role = id > 100 ? id - 100 : id;
    const auto it = std::find(kSeats.begin(), kSeats.end(), role);
    return it == kSeats.end() ? -1 : static_cast<int>(it - kSeats.begin());
}

// One deterministic port per competition, team and directed seat pair.
// The receiving computer's physical address is independent of its logged-in role.
std::uint16_t SeatPort(app::Competition competition, int sender, int receiver) {
    return static_cast<std::uint16_t>(47710 + 18 * CompetitionCode(competition)
                                    + (sender > 100 ? 9 : 0) + 3 * SeatIndex(sender) + SeatIndex(receiver));
}

Handshake MakeHandshake(const std::optional<TeamSeatPair> &seats, bool inverted) {
    Handshake packet{};
    WriteU32BE(packet.data(), kHandshakeMagic);
    packet[4] = seats ? kSeatVersion : kHandshakeVersion;
    packet[5] = inverted ? kFlagInverted : 0;
    if (seats) {
        packet[5] |= static_cast<std::uint8_t>(CompetitionCode(seats->competition) << 1);
        packet[6] = static_cast<std::uint8_t>(seats->local_robot_id);
        packet[7] = static_cast<std::uint8_t>(seats->remote_robot_id);
    }
    return packet;
}

bool MatchesPeer(const Handshake &packet, const std::optional<TeamSeatPair> &seats) {
    if (ReadU32BE(packet.data()) != kHandshakeMagic) return false;
    if (!seats) return packet[4] == kHandshakeVersion;
    return packet[4] == kSeatVersion && (packet[5] >> 1) == CompetitionCode(seats->competition)
        && packet[6] == seats->remote_robot_id && packet[7] == seats->local_robot_id;
}

using IPv4 = boost::asio::ip::address_v4;

std::optional<IPv4> LocalSeatAddress(boost::asio::io_context &io, const TeamSeatPair &seats) {
    if (seats.loopback) return IPv4::loopback();
    for (const auto *host : kTeamHosts) {
        Error ec;
        TcpSocket probe(io);
        probe.open(tcp::v4(), ec);
        if (ec) continue;
        const auto address = boost::asio::ip::make_address_v4(host);
        probe.bind({address, 0}, ec);
        if (!ec) return address;
    }
    return {};
}

bool IsSeatAddress(const boost::asio::ip::address &address, const TeamSeatPair &seats) {
    if (!address.is_v4()) return false;
    if (seats.loopback) return address.to_v4() == IPv4::loopback();
    return std::ranges::any_of(kTeamHosts, [&](const char *host) {
        return address.to_v4() == boost::asio::ip::make_address_v4(host);
    });
}

// stop() may interrupt a blocking socket operation, but only its owning thread closes it.
struct SocketInterrupt {
    using Handle = decltype(std::declval<TcpSocket &>().native_handle());
    std::mutex mutex;
    std::optional<Handle> handle;
    void interrupt() {
        std::lock_guard lock(mutex);
        if (handle) ::shutdown(*handle, static_cast<int>(tcp::socket::shutdown_both));
    }
    struct Scope {
        SocketInterrupt &owner;
        Scope(SocketInterrupt &slot, TcpSocket &socket) : owner(slot) {
            std::lock_guard lock(owner.mutex);
            owner.handle = socket.native_handle();
        }
        ~Scope() {
            std::lock_guard lock(owner.mutex);
            owner.handle.reset();
        }
    };
};

}// namespace

std::vector<int> TeamSeatIds(app::Competition competition, int local_robot_id) {
    const int self = SeatIndex(local_robot_id);
    const int count = competition == app::Competition::Ul2027 ? 2 : 3;
    if (self < 0 || self >= count || CompetitionCode(competition) < 0) return {};
    std::vector<int> result;
    for (int slot = 0; slot < count; ++slot) {
        if (slot != self) result.push_back(kSeats[slot] + (local_robot_id > 100 ? 100 : 0));
    }
    return result;
}

const char *TeamSeatName(app::Competition competition, int robot_id) {
    switch (SeatIndex(robot_id)) {
        case 0: return competition == app::Competition::Uc2026 ? "英雄 1 号" : "重装 1 号";
        case 1: return "步兵 3 号";
        case 2: return "步兵 4 号";
        default: return "未分配岗位";
    }
}

TeammateLinkConfig TeamSeatLinkConfig(app::Competition competition, int local_robot_id,
                                      int remote_robot_id, bool loopback) {
    TeammateLinkConfig config;
    const auto teammates = TeamSeatIds(competition, local_robot_id);
    if (std::find(teammates.begin(), teammates.end(), remote_robot_id) == teammates.end()) return config;
    config.seats = TeamSeatPair{competition, local_robot_id, remote_robot_id, loopback};
    config.listen_port = SeatPort(competition, remote_robot_id, local_robot_id);
    config.peer.port = SeatPort(competition, local_robot_id, remote_robot_id);
    config.connect_timeout = std::chrono::milliseconds(400);
    config.reconnect_delay = std::chrono::milliseconds(220);
    return config;
}

struct TeammateLink::Shared {
    TeammateLinkConfig config;
    FrameHandler on_frame;
    std::atomic<bool> running{true}, inverted{false}, connected{false}, served{false}, remote_inverted{false};
    std::atomic<std::uint64_t> enqueued{0}, sent{0}, dropped{0}, tx_bytes{0}, received{0}, rx_bytes{0};
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Bytes> queue;
    ParameterSets parameter_sets;
    TeammateEndpoint peer;
    std::string remote_address;
    std::atomic<unsigned> generation{0};
    SocketInterrupt tx, rx;

    void pause(std::chrono::milliseconds duration) {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, duration, [&] { return !running.load(); });
    }
};

TeammateLink::TeammateLink(TeammateLinkConfig config, FrameHandler on_frame) : shared_(std::make_shared<Shared>()) {
    shared_->config = std::move(config);
    shared_->config.send_queue_capacity = std::clamp<std::size_t>(shared_->config.send_queue_capacity, 1, 16);
    shared_->on_frame = std::move(on_frame);
    shared_->peer = shared_->config.peer;
    auto guarded = [this](auto fn) {
        try { (this->*fn)(); }
        catch (const std::exception &ex) { SPDLOG_ERROR("队友视频线程停止 {}", ex.what()); }
    };
    try {
        if (shared_->config.listen_port) receiver_thread_ = std::thread(guarded, &TeammateLink::run_receiver);
        if (shared_->peer.port && (shared_->config.seats || !shared_->peer.host.empty()))
            sender_thread_ = std::thread(guarded, &TeammateLink::run_sender);
    } catch (...) {
        stop();
        throw;
    }
}

TeammateLink::~TeammateLink() { stop(); }

void TeammateLink::push_frame(const Bytes &frame) {
    if (frame.empty() || frame.size() > kMaxPayloadBytes || !shared_->running) return;
    {
        std::lock_guard lock(shared_->mutex);
        if (!shared_->running) return;
        CacheHeaders(frame, shared_->parameter_sets);
        if (shared_->queue.size() >= shared_->config.send_queue_capacity) {
            shared_->queue.pop_front();
            ++shared_->dropped;
        }
        shared_->queue.push_back(frame);
        ++shared_->enqueued;
    }
    shared_->cv.notify_all();
}

void TeammateLink::set_local_inverted(bool inverted) { shared_->inverted = inverted; }

void TeammateLink::set_peer(TeammateEndpoint peer) {
    if (shared_->config.seats) return;
    {
        std::lock_guard lock(shared_->mutex);
        shared_->peer = std::move(peer);
        ++shared_->generation;
        shared_->dropped += shared_->queue.size();
        shared_->queue.clear();
    }
    shared_->tx.interrupt();
    shared_->cv.notify_all();
}

TeammateLinkStats TeammateLink::stats() const {
    std::lock_guard lock(shared_->mutex);
    TeammateLinkStats out;
    out.frames_queued = shared_->enqueued;
    out.frames_sent = shared_->sent;
    out.frames_dropped = shared_->dropped;
    out.bytes_sent = shared_->tx_bytes;
    out.queued_now = shared_->queue.size();
    out.peer_connected = shared_->connected;
    out.frames_received = shared_->received;
    out.bytes_received = shared_->rx_bytes;
    out.peer_served = shared_->served;
    out.peer_frame_inverted = shared_->remote_inverted;
    out.peer_address = shared_->remote_address;
    return out;
}

void TeammateLink::stop() {
    if (stopped_) return;
    stopped_ = true;
    shared_->running = false;
    shared_->cv.notify_all();
    shared_->rx.interrupt();
    shared_->tx.interrupt();
    if (sender_thread_.joinable()) sender_thread_.join();
    if (receiver_thread_.joinable()) receiver_thread_.join();
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
    auto &s = *shared_;
    const auto &seats = s.config.seats;
    boost::asio::io_context io;

    std::size_t candidate = 0;
    while (s.running) {
        TeammateEndpoint peer;
        unsigned generation;
        {
            std::lock_guard lock(s.mutex);
            peer = s.peer;
            generation = s.generation;
        }
        TcpSocket socket(io);
        Error error;
        if (seats) {
            const auto local = LocalSeatAddress(io, *seats);
            if (!local) {
                s.pause(s.config.reconnect_delay);
                continue;
            }
            if (seats->loopback) {
                peer.host = "127.0.0.1";
            } else {
                do { peer.host = kTeamHosts[candidate++ % kTeamHosts.size()]; } while (peer.host == local->to_string());
            }
            socket.open(tcp::v4(), error);
            if (!error) socket.set_option(tcp::socket::do_not_route(true), error);
            if (!error) socket.bind({*local, 0}, error);
            if (error) {
                s.pause(s.config.reconnect_delay);
                continue;
            }
        }
        const auto address = boost::asio::ip::make_address(peer.host, error);
        if (error || !ConnectWithTimeout(io, socket, {address, peer.port}, s.config.connect_timeout)) {
            s.pause(s.config.reconnect_delay);
            continue;
        }
        {
            SocketInterrupt::Scope active(s.tx, socket);
            if (!s.running) break;
            socket.set_option(tcp::no_delay(true), error);
            ApplySocketTimeouts(socket, kSocketTimeoutMs);
            const auto hello = MakeHandshake(seats, s.inverted);
            bool ok = WriteAll(socket, hello.data(), hello.size());
            if (ok && seats) {
                Handshake reply;
                ok = ReadExact(socket, reply.data(), reply.size()) && MatchesPeer(reply, seats);
            }
            if (ok) {
                s.connected = true;
                SPDLOG_INFO("队友视频发送已连接 {}:{} 岗位={}", peer.host, peer.port,
                            seats ? seats->remote_robot_id : 0);
            }
            // Send the complete VPS/SPS/PPS together. Resetting a receiver on each
            // separate set would throw away the preceding set of the same stream.
            if (ok) {
                Bytes header;
                {
                    std::lock_guard lock(s.mutex);
                    for (const auto &part : s.parameter_sets) header.insert(header.end(), part.begin(), part.end());
                }
                if (!header.empty()) ok = WriteFrame(socket, header, s.inverted ? kFlagInverted : 0);
            }
            auto last_write = std::chrono::steady_clock::now();
            while (ok && s.running && generation == s.generation) {
                Bytes frame;
                {
                    std::unique_lock lock(s.mutex);
                    s.cv.wait_for(lock, kSendPopTimeout, [&] {
                        return !s.queue.empty() || !s.running || generation != s.generation;
                    });
                    if (!s.running || generation != s.generation) break;
                    if (!s.queue.empty()) {
                        frame = std::move(s.queue.front());
                        s.queue.pop_front();
                    }
                }
                if (frame.empty()) {
                    // An idle camera keeps its seat connection. A heartbeat is
                    // consumed at the receiver and never reaches video decoding.
                    if (!seats || std::chrono::steady_clock::now() - last_write < std::chrono::milliseconds(700)) continue;
                }
                ok = WriteFrame(socket, frame, s.inverted ? kFlagInverted : 0);
                if (ok) {
                    last_write = std::chrono::steady_clock::now();
                    if (!frame.empty()) {
                        ++s.sent;
                        s.tx_bytes += frame.size();
                    }
                }
            }
            s.connected = false;
        }
        s.pause(s.config.reconnect_delay);
    }
    s.connected = false;
}

void TeammateLink::run_receiver() {
    auto &s = *shared_;
    const auto &seats = s.config.seats;
    boost::asio::io_context io;
    while (s.running) {
        std::optional<IPv4> local = seats ? LocalSeatAddress(io, *seats) : std::optional<IPv4>(IPv4::any());
        if (!local) {
            s.pause(s.config.reconnect_delay);
            continue;
        }
        tcp::acceptor acceptor(io);
        Error error;
        // A seat's port must exactly match the agreed direction. Manual legacy
        // connections retain their existing fallback to the next available port.
        std::uint32_t port = s.config.listen_port;
        const std::uint32_t last = std::min<std::uint32_t>(65535, port + (seats ? 0 : 9));
        for (; port <= last; ++port) {
            acceptor.open(tcp::v4(), error);
#ifdef _WIN32
            if (!error) {
                const BOOL exclusive = TRUE;
                if (::setsockopt(acceptor.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                                 reinterpret_cast<const char *>(&exclusive), sizeof(exclusive)) != 0)
                    error.assign(WSAGetLastError(), boost::asio::error::get_system_category());
            }
#else
            if (!error) acceptor.set_option(tcp::acceptor::reuse_address(true), error);
#endif
            if (!error) acceptor.bind({*local, static_cast<std::uint16_t>(port)}, error);
            if (!error) acceptor.listen(4, error);
            if (!error) acceptor.non_blocking(true, error);
            if (!error) break;
            Error ignored;
            acceptor.close(ignored);
        }
        if (port > last) {
            s.pause(s.config.reconnect_delay);
            continue;
        }
        SPDLOG_INFO("队友视频监听 {}:{} 岗位={}", local->to_string(), port, seats ? seats->remote_robot_id : 0);
        while (s.running) {
            TcpSocket socket(io);
            acceptor.accept(socket, error);
            if (error) {
                if (error != boost::asio::error::would_block && error != boost::asio::error::try_again) break;
                s.pause(std::chrono::milliseconds(30));
                continue;
            }
            const auto remote = socket.remote_endpoint(error);
            if (error || (seats && !IsSeatAddress(remote.address(), *seats))) continue;
            SocketInterrupt::Scope active(s.rx, socket);
            if (!s.running) break;
            socket.non_blocking(false, error);
            socket.set_option(tcp::no_delay(true), error);
            ApplySocketTimeouts(socket, kSocketTimeoutMs);
            Handshake hello;
            if (!ReadExact(socket, hello.data(), hello.size()) || !MatchesPeer(hello, seats)) continue;
            if (seats) {
                const auto reply = MakeHandshake(seats, s.inverted);
                if (!WriteAll(socket, reply.data(), reply.size())) continue;
            }
            {
                std::lock_guard lock(s.mutex);
                s.remote_address = remote.address().to_string() + ":" + std::to_string(remote.port());
                s.remote_inverted = (hello[5] & kFlagInverted) != 0;
                s.served = true;
            }
            SPDLOG_INFO("队友视频接收已配对 {} 岗位={}", remote.address().to_string(), seats ? seats->remote_robot_id : 0);
            std::uint8_t header[8];
            while (s.running && ReadExact(socket, header, sizeof(header))) {
                const std::uint32_t size = ReadU32BE(header);
                if (size == 0 && seats) continue;
                if (size == 0 || size > kMaxPayloadBytes) break;
                Bytes frame(size);
                if (!ReadExact(socket, frame.data(), frame.size()) || !s.running) break;
                s.remote_inverted = (ReadU32BE(header + 4) & kFlagInverted) != 0;
                ++s.received;
                s.rx_bytes += size;
                if (s.on_frame) s.on_frame(std::move(frame));
            }
            {
                std::lock_guard lock(s.mutex);
                s.served = false;
                s.remote_address.clear();
                s.remote_inverted = false;
            }
        }
    }
}
}// namespace RoboGo::net
