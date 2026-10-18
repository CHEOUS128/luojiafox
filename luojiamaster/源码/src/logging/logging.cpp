#include "logging.h"
#include "embedded_messages_proto.h"
#include "network_capture_format.h"

namespace RoboGo::logging {

namespace {

struct NetworkCaptureRecord {
    std::uint64_t timestamp_us = 0;
    std::uint8_t transport = 0;
    std::uint8_t direction = 0;
    std::string channel;
    std::string peer;
    std::vector<std::uint8_t> payload;
};

std::once_flag g_initialize_once;
std::filesystem::path g_log_root_directory;
std::filesystem::path g_log_directory;
std::filesystem::path g_network_capture_path;
std::string g_session_id;
std::ofstream g_network_capture_stream;
std::thread g_network_capture_thread;
constexpr std::size_t kNetworkCaptureQueueSize = 8192;
moodycamel::BlockingReaderWriterQueue<NetworkCaptureRecord> g_network_capture_queue{kNetworkCaptureQueueSize};
std::size_t g_records_since_flush = 0;
std::atomic<bool> g_network_stop_requested = false;
bool g_is_initialized = false;

constexpr std::array<char, 8> kCaptureMagic = {'R', 'B', 'N', 'C', 'A', 'P', '1', '\n'};
constexpr std::array<char, 4> kRecordMagic = {'R', 'E', 'C', '2'};
constexpr std::uint32_t kCaptureVersion = 2;
constexpr std::size_t kSpdlogQueueSize = 8192;
constexpr auto kNetworkCaptureWaitTimeout = std::chrono::milliseconds(1);
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

std::uint32_t crc32(std::span<const std::uint8_t> data) {
    static std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> generated{};
        for (std::uint32_t index = 0; index < generated.size(); ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                if ((value & 1U) != 0U) {
                    value = 0xEDB88320U ^ (value >> 1U);
                } else {
                    value >>= 1U;
                }
            }
            generated[index] = value;
        }
        return generated;
    }();

    std::uint32_t value = 0xFFFFFFFFU;
    for (const std::uint8_t byte : data) {
        value = table[(value ^ byte) & 0xFFU] ^ (value >> 8U);
    }
    return value ^ 0xFFFFFFFFU;
}

std::vector<std::uint8_t> build_record_body(const NetworkCaptureRecord &record) {
    const auto channel_length = static_cast<std::uint16_t>(std::min<std::size_t>(record.channel.size(), std::numeric_limits<std::uint16_t>::max()));
    const auto peer_length = static_cast<std::uint16_t>(std::min<std::size_t>(record.peer.size(), std::numeric_limits<std::uint16_t>::max()));
    const auto payload_length = static_cast<std::uint32_t>(std::min<std::size_t>(record.payload.size(), std::numeric_limits<std::uint32_t>::max()));

    std::vector<std::uint8_t> body;
    body.reserve(sizeof(std::uint64_t) + sizeof(std::uint8_t) * 2 + sizeof(std::uint16_t) * 2 + sizeof(std::uint32_t)
                 + channel_length + peer_length + payload_length);
    append_little_endian<std::uint64_t>(body, record.timestamp_us);
    body.push_back(record.transport);
    body.push_back(record.direction);
    append_little_endian<std::uint16_t>(body, channel_length);
    append_little_endian<std::uint16_t>(body, peer_length);
    append_little_endian<std::uint32_t>(body, payload_length);
    body.insert(body.end(), record.channel.begin(), record.channel.begin() + static_cast<std::ptrdiff_t>(channel_length));
    body.insert(body.end(), record.peer.begin(), record.peer.begin() + static_cast<std::ptrdiff_t>(peer_length));
    body.insert(body.end(), record.payload.begin(), record.payload.begin() + static_cast<std::ptrdiff_t>(payload_length));
    return body;
}

void write_record(std::ofstream &stream, const NetworkCaptureRecord &record) {
    const std::vector<std::uint8_t> body = build_record_body(record);
    const std::uint32_t body_size = static_cast<std::uint32_t>(body.size());
    const std::uint32_t body_crc = crc32(body);

    stream.write(kRecordMagic.data(), static_cast<std::streamsize>(kRecordMagic.size()));
    write_little_endian<std::uint32_t>(stream, body_size);
    write_little_endian<std::uint32_t>(stream, body_crc);
    stream.write(reinterpret_cast<const char *>(body.data()), static_cast<std::streamsize>(body.size()));
}

void flush_capture_stream(bool force_flush) {
    if (!g_network_capture_stream.is_open()) {
        return;
    }
    if (force_flush || g_records_since_flush >= 32) {
        g_network_capture_stream.flush();
        g_records_since_flush = 0;
    }
}

void network_capture_loop() {
    SPDLOG_INFO("网络抓包线程已启动");

    NetworkCaptureRecord record;
    for (;;) {
        if (!g_network_capture_queue.wait_dequeue_timed(record, kNetworkCaptureWaitTimeout)) {
            if (g_network_stop_requested.load(std::memory_order_acquire)) {
                break;
            }
            continue;
        }

        write_record(g_network_capture_stream, record);
        g_records_since_flush += 1;

        while (g_network_capture_queue.try_dequeue(record)) {
            write_record(g_network_capture_stream, record);
            g_records_since_flush += 1;
        }
        flush_capture_stream(false);
    }

    flush_capture_stream(true);
    if (g_network_capture_stream.is_open()) {
        g_network_capture_stream.close();
    }
    SPDLOG_INFO("网络抓包线程已退出");
}

}// namespace

void initialize(std::string_view application_name, const bool invert_udp_video) {
    std::call_once(g_initialize_once, [application_name, invert_udp_video]() {
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
                spdlog::async_overflow_policy::block);
        app_logger->set_level(spdlog::level::trace);
        app_logger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(app_logger);

        g_network_capture_stream.open(g_network_capture_path, std::ios::binary | std::ios::trunc);
        if (!g_network_capture_stream.is_open()) {
            throw std::runtime_error("无法打开网络抓包文件");
        }
        g_network_capture_stream.write(kCaptureMagic.data(), static_cast<std::streamsize>(kCaptureMagic.size()));
        write_little_endian<std::uint32_t>(g_network_capture_stream, kCaptureVersion);
        const std::uint32_t capture_flags = BuildNetworkCaptureFlags(invert_udp_video);
        write_little_endian<std::uint32_t>(g_network_capture_stream, capture_flags);
        g_network_capture_stream.flush();

        g_network_stop_requested.store(false, std::memory_order_release);
        g_records_since_flush = 0;
        g_network_capture_thread = std::thread(network_capture_loop);
        g_is_initialized = true;

        SPDLOG_INFO("日志系统已初始化 application={} session={} log_root={} log_dir={}",
                    application_name,
                    g_session_id,
                    g_log_root_directory.string(),
                    g_log_directory.string());
        SPDLOG_INFO("应用日志已切换为异步模式 thread_count=1 queue_size={}", kSpdlogQueueSize);
        SPDLOG_INFO("原始网络抓包已启用 path={} format=binary-v2 flags={} invert_udp_video={}",
                    g_network_capture_path.string(),
                    capture_flags,
                    invert_udp_video);
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
    if (!g_is_initialized || g_network_stop_requested.load(std::memory_order_acquire)) {
        return;
    }

    NetworkCaptureRecord record;
    const auto timestamp = std::chrono::system_clock::now();
    record.timestamp_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(timestamp.time_since_epoch()).count());
    record.transport = encode_transport(transport);
    record.direction = encode_direction(direction);
    record.channel.assign(channel);
    record.peer.assign(peer);
    record.payload.assign(payload.begin(), payload.end());

    if (!g_network_capture_queue.emplace(std::move(record))) {
        SPDLOG_WARN("网络抓包队列已满，丢弃记录 channel={} peer={} bytes={}",
                    channel,
                    peer,
                    payload.size());
    }
}

}// namespace RoboGo::logging
