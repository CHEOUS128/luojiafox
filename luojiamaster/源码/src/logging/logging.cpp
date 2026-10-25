#include "logging.h"
#include "embedded_messages_proto.h"
#include "network_capture_format.h"

namespace RoboGo::logging {

namespace {

// 抓包记录在入队时就已经是整个「要落盘的字节块」，结构：
//   0  u32 body_size   (= channel_len + peer_len + payload_len)
//   4  u64 timestamp_us
//  12  u8  transport
//  13  u8  direction
//  14  u16 channel_len
//  16  u16 peer_len
//  18  u32 payload_len
//  22  channel | peer | payload
//   —— 共 26 字节头，没有逐条 magic/CRC。
// 和旧版 v2 相比去掉了逐条 CRC：每秒 6 万条时算 CRC 才是真正的瓶颈，
// 而这个文件只用于事后回放分析，少一层校验换来“不拖垮网络线程”是划算的。
constexpr std::size_t kNetworkCaptureRecordHeaderSize = 26;

inline void store_u16(std::uint8_t *dst, const std::uint16_t value) {
    dst[0] = static_cast<std::uint8_t>(value & 0xFFU);
    dst[1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

inline void store_u32(std::uint8_t *dst, const std::uint32_t value) {
    for (int index = 0; index < 4; ++index) {
        dst[index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFU);
    }
}

inline void store_u64(std::uint8_t *dst, const std::uint64_t value) {
    for (int index = 0; index < 8; ++index) {
        dst[index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFU);
    }
}

inline std::uint32_t load_u32(const std::uint8_t *src) {
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(src[index]) << (index * 8);
    }
    return value;
}

std::once_flag g_initialize_once;
std::filesystem::path g_log_root_directory;
std::filesystem::path g_log_directory;
std::filesystem::path g_network_capture_path;
std::string g_session_id;
std::ofstream g_network_capture_stream;
std::thread g_network_capture_thread;
// 32K 条 ≈ 0.5 秒的 UDP 视频：抓包线程偶尔卡一下也不会丢记录。
// 队列里存的是裸内存块（每块一次 malloc），不再有 std::string / std::vector 的额外拷贝。
constexpr std::size_t kNetworkCaptureQueueSize = 32768;
moodycamel::BlockingReaderWriterQueue<std::uint8_t *> g_network_capture_queue{kNetworkCaptureQueueSize};
std::atomic<bool> g_network_stop_requested = false;
// --no_capture 时彻底不抓包，用来做 A/B 对比。
std::atomic<bool> g_network_capture_enabled = true;
// 队列满了以后的丢弃计数：只统计，绝不阻塞调用线程。
std::atomic<std::size_t> g_network_capture_dropped = 0;
bool g_is_initialized = false;

constexpr std::array<char, 8> kCaptureMagic = {'R', 'B', 'N', 'C', 'A', 'P', '1', '\n'};
constexpr std::uint32_t kHeaderVersion = 3;
constexpr std::size_t kSpdlogQueueSize = 8192;
constexpr std::uint8_t kTransportUnknown = 0;
constexpr std::uint8_t kTransportMqtt = 1;
constexpr std::uint8_t kTransportUdpVideo = 2;
constexpr std::uint8_t kDirectionUnknown = 0;
constexpr std::uint8_t kDirectionRx = 1;
constexpr std::uint8_t kDirectionTx = 2;
constexpr std::string_view kDefaultLogRootDirectory = "logs";

std::tm local_time(std::time_t time_point) {
    std::tm result{};
#ifdef _WIN32
    localtime_s(&result, &time_point);
#else
    localtime_r(&time_point, &result);
#endif
    return result;
}

std::string make_session_id() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time_point = std::chrono::system_clock::to_time_t(now);
    const std::tm local_tm = local_time(time_point);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::ostringstream stream;
    stream << std::put_time(&local_tm, "%Y%m%d_%H%M%S")
           << "_"
           << std::setw(3)
           << std::setfill('0')
           << milliseconds.count();
    return stream.str();
}

std::filesystem::path resolve_log_root_directory() {
    if (const char *env_value = std::getenv("ROBOGO_LOG_ROOT"); env_value != nullptr && env_value[0] != '\0') {
        return std::filesystem::path(env_value);
    }
    return std::filesystem::path(kDefaultLogRootDirectory);
}

std::uint8_t encode_transport(std::string_view transport) {
    if (transport == "mqtt") {
        return kTransportMqtt;
    }
    if (transport == "udp-video") {
        return kTransportUdpVideo;
    }
    return kTransportUnknown;
}

std::uint8_t encode_direction(std::string_view direction) {
    if (direction == "rx") {
        return kDirectionRx;
    }
    if (direction == "tx") {
        return kDirectionTx;
    }
    return kDirectionUnknown;
}

template<typename Integer>
void append_little_endian(std::vector<std::uint8_t> &buffer, Integer value) {
    static_assert(std::is_integral_v<Integer>, "Integer type required");

    using UnsignedInteger = std::make_unsigned_t<Integer>;
    UnsignedInteger unsigned_value = static_cast<UnsignedInteger>(value);
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        buffer.push_back(static_cast<std::uint8_t>((unsigned_value >> (index * 8)) & 0xFF));
    }
}

template<typename Integer>
void write_little_endian(std::ofstream &stream, Integer value) {
    std::vector<std::uint8_t> buffer;
    buffer.reserve(sizeof(Integer));
    append_little_endian(buffer, value);
    stream.write(reinterpret_cast<const char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
}

// 存盘格式（v3）：magic "RBNCAP1\n" + u32 版本 + u32 flags，
// 之后是连续的记录块，每条 = 26 字节头 + channel/peer/payload，没有逐条 CRC。
std::uint8_t *build_capture_record(const std::uint64_t timestamp_us,
                                   const std::uint8_t transport,
                                   const std::uint8_t direction,
                                   const std::string_view channel,
                                   const std::string_view peer,
                                   const std::span<const std::uint8_t> payload) {
    const std::size_t channel_length = std::min<std::size_t>(channel.size(), 0xFFFFU);
    const std::size_t peer_length = std::min<std::size_t>(peer.size(), 0xFFFFU);
    const std::size_t payload_length = std::min<std::size_t>(payload.size(), 0xFFFFFFFFU);
    const std::size_t body_length = channel_length + peer_length + payload_length;

    auto *record = static_cast<std::uint8_t *>(std::malloc(kNetworkCaptureRecordHeaderSize + body_length));
    if (record == nullptr) {
        return nullptr;
    }

    store_u32(record, static_cast<std::uint32_t>(body_length));
    store_u64(record + 4, timestamp_us);
    record[12] = transport;
    record[13] = direction;
    store_u16(record + 14, static_cast<std::uint16_t>(channel_length));
    store_u16(record + 16, static_cast<std::uint16_t>(peer_length));
    store_u32(record + 18, static_cast<std::uint32_t>(payload_length));
    std::uint8_t *cursor = record + kNetworkCaptureRecordHeaderSize;
    std::memcpy(cursor, channel.data(), channel_length);
    cursor += channel_length;
    std::memcpy(cursor, peer.data(), peer_length);
    cursor += peer_length;
    if (payload_length > 0) {
        std::memcpy(cursor, payload.data(), payload_length);
    }
    return record;
}

void network_capture_loop() {
    SPDLOG_INFO("网络抓包线程已启动");

    std::uint8_t *record = nullptr;
    std::size_t unflushed_bytes = 0;
    auto last_flush_time = std::chrono::steady_clock::now();
    for (;;) {
        // 一次最多阻塞 100 ms。旧版是 1 ms 轮询 + 每 32 条就 flush，抓包线程自己占满核。
        if (!g_network_capture_queue.wait_dequeue_timed(record, std::chrono::milliseconds(100))) {
            if (g_network_stop_requested.load(std::memory_order_acquire)) {
                break;
            }
            continue;
        }

        // 把已经攒下的记录一次写进文件。记录块是自描述的：先写 4 字节 body_size。
        const std::uint32_t body_size = load_u32(record);
        const auto record_size = static_cast<std::size_t>(body_size) + kNetworkCaptureRecordHeaderSize;
        g_network_capture_stream.write(reinterpret_cast<const char *>(record), static_cast<std::streamsize>(record_size));
        unflushed_bytes += record_size;
        std::free(record);
        while (g_network_capture_queue.try_dequeue(record)) {
            const std::uint32_t next_body_size = load_u32(record);
            const auto next_record_size = static_cast<std::size_t>(next_body_size) + kNetworkCaptureRecordHeaderSize;
            g_network_capture_stream.write(reinterpret_cast<const char *>(record), static_cast<std::streamsize>(next_record_size));
            unflushed_bytes += next_record_size;
            std::free(record);
        }

        // 攒够 256 KB 或过了 200 ms 就落盘：既不会每条都 flush 占满磁盘，
        // 又能保证进程被强制结束时，抓包文件里留住出事那一刻的数据。
        const auto now = std::chrono::steady_clock::now();
        if (unflushed_bytes >= 256 * 1024 || now - last_flush_time >= std::chrono::milliseconds(200)) {
            g_network_capture_stream.flush();
            unflushed_bytes = 0;
            last_flush_time = now;
        }
    }

    g_network_capture_stream.flush();
    g_network_capture_stream.close();
    SPDLOG_INFO("网络抓包线程已退出");
}

}// namespace

void initialize(std::string_view application_name, const bool invert_udp_video, const bool enable_network_capture) {
    std::call_once(g_initialize_once, [application_name, invert_udp_video, enable_network_capture]() {
        g_log_root_directory = resolve_log_root_directory();
        g_session_id = make_session_id();
        g_log_directory = g_log_root_directory / g_session_id;

        std::filesystem::create_directories(g_log_root_directory);
        std::filesystem::create_directories(g_log_directory);

        const auto app_log_path = g_log_directory / "robogo.log";
        g_network_capture_path = g_log_directory / "network_capture.bin";

        // 保存当前 proto 定义到日志目录，用于后续日志分析
        // 即使 proto 文件后续变更，旧日志仍可基于保存的 proto 进行解析
        {
            const auto proto_path = g_log_directory / "messages.proto";
            std::ofstream proto_stream(proto_path, std::ios::binary | std::ios::trunc);
            if (proto_stream.is_open()) {
                proto_stream.write(reinterpret_cast<const char *>(embedded_messages_proto_data),
                                   static_cast<std::streamsize>(embedded_messages_proto_data_len));
                proto_stream.close();
                SPDLOG_INFO("已保存 proto 定义到日志目录 path={} size={}B",
                            proto_path.string(), embedded_messages_proto_data_len);
            } else {
                SPDLOG_WARN("无法写入 proto 文件到日志目录 path={}", proto_path.string());
            }
        }

        auto app_file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(app_log_path.string(), true);
        app_file_sink->set_pattern("%Y-%m-%d %H:%M:%S.%e [%^%l%$] %v");

        std::vector<spdlog::sink_ptr> sinks{app_file_sink};
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern("%Y-%m-%d %H:%M:%S.%e [%^%l%$] %v");
        sinks.push_back(console_sink);

        spdlog::init_thread_pool(kSpdlogQueueSize, 1);
        auto app_logger = std::make_shared<spdlog::async_logger>(
                "robogo",
                sinks.begin(),
                sinks.end(),
                spdlog::thread_pool(),
                // 绝不能因为写日志把主线程/网络线程卡住：队列满时丢最旧的，保留最新的现场。
                spdlog::async_overflow_policy::overrun_oldest);
        app_logger->set_level(spdlog::level::trace);
        // 不再 flush_on(info)：异步队列被塞满时会 flush，等于又变回同步写盘。
        // 日志由 spdlog 的周期 flush 保证，崩溃时最多丢最后 3 秒。
        spdlog::flush_every(std::chrono::seconds(3));
        spdlog::set_default_logger(app_logger);

        g_network_capture_stream.open(g_network_capture_path, std::ios::binary | std::ios::trunc);
        if (!g_network_capture_stream.is_open()) {
            throw std::runtime_error("无法打开网络抓包文件");
        }
        g_network_capture_stream.write(kCaptureMagic.data(), static_cast<std::streamsize>(kCaptureMagic.size()));
        write_little_endian<std::uint32_t>(g_network_capture_stream, kHeaderVersion);
        const std::uint32_t capture_flags = BuildNetworkCaptureFlags(invert_udp_video);
        write_little_endian<std::uint32_t>(g_network_capture_stream, capture_flags);
        g_network_capture_stream.flush();

        g_network_stop_requested.store(false, std::memory_order_release);
        g_network_capture_enabled.store(enable_network_capture, std::memory_order_release);
        if (enable_network_capture) {
            g_network_capture_thread = std::thread(network_capture_loop);
        }
        g_is_initialized = true;

        SPDLOG_INFO("日志系统已初始化 application={} session={} log_root={} log_dir={}",
                    application_name,
                    g_session_id,
                    g_log_root_directory.string(),
                    g_log_directory.string());
        SPDLOG_INFO("应用日志已切换为异步模式 thread_count=1 queue_size={}", kSpdlogQueueSize);
        if (enable_network_capture) {
            SPDLOG_INFO("原始网络抓包已启用 path={} format=binary-v3 flags={} invert_udp_video={}",
                        g_network_capture_path.string(),
                        capture_flags,
                        invert_udp_video);
        } else {
            SPDLOG_WARN("原始网络抓包已按启动参数关闭（--no_capture）");
        }
    });
}

void shutdown() {
    if (!g_is_initialized) {
        return;
    }
    SPDLOG_INFO("日志系统准备关闭 session={}", g_session_id);
    g_network_stop_requested.store(true, std::memory_order_release);
    if (g_network_capture_thread.joinable()) {
        g_network_capture_thread.join();
    }
    spdlog::shutdown();
    g_is_initialized = false;
}

bool is_initialized() {
    return g_is_initialized;
}

const std::filesystem::path &log_root_directory() {
    return g_log_root_directory;
}

const std::filesystem::path &log_directory() {
    return g_log_directory;
}

const std::filesystem::path &network_capture_path() {
    return g_network_capture_path;
}

const std::string &session_id() {
    return g_session_id;
}

std::string truncate_text(std::string_view text, std::size_t max_length) {
    if (text.size() <= max_length) {
        return std::string(text);
    }
    std::string truncated(text.substr(0, max_length));
    truncated += "...(truncated)";
    return truncated;
}

void log_network_payload(std::string_view transport,
                         std::string_view direction,
                         std::string_view channel,
                         std::span<const std::uint8_t> payload,
                         std::string_view peer) {
    if (!g_is_initialized || !g_network_capture_enabled.load(std::memory_order_acquire)
        || g_network_stop_requested.load(std::memory_order_acquire)) {
        return;
    }

    const auto timestamp = std::chrono::system_clock::now();
    const auto timestamp_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(timestamp.time_since_epoch()).count());

    // 在调用线程上拼「一条记录」的裸字节块：一次 malloc + 一次 memcpy，
    // 不再有 std::string / std::vector 的额外分配和拷贝。
    std::uint8_t *record = build_capture_record(timestamp_us,
                                                encode_transport(transport),
                                                encode_direction(direction),
                                                channel,
                                                peer,
                                                payload);

    // 抓包线程是唯一的写者，调用线程只入队：绝不在网络/主线程上碰文件。
    if (record == nullptr || !g_network_capture_queue.try_enqueue(record)) {
        std::free(record);
        g_network_capture_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

std::size_t network_capture_dropped_count() {
    return g_network_capture_dropped.load(std::memory_order_relaxed);
}

}// namespace RoboGo::logging
