#include "video_stability_test.h"

#include "app/latest_video_frame.h"
#include "app/video_performance_trace.h"
#include "video/rgba_frame_converter.h"
#include "keyboard_mouse_state_manager.h"
#include "logging/logging.h"
#include "platform/platform_backend.h"
#include "platform/platform_util.h"
#include "resource/resources.h"
#include "video/custom_byte_block_video_decoder.h"
#include "video/video_decoder.h"
#include "video/video_shards_manager.h"
extern "C" {
#include <libavutil/sha.h>
}
#ifdef _WIN32
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

namespace RoboGo::app {
namespace {
using Clock = std::chrono::steady_clock;
using performance::DataArrival;
using performance::FrameTrace;

struct Options {
    std::string capture;
    std::string report_dir;
    double seconds = 1800;
    double replay_window_seconds = 0;
    bool exercise_ui = true;
    bool performance_baseline = false;
    bool input_probe = false;
    double ui_work_ms = 0;
    std::string input_placement = "profile";
    bool early_input = false;
    std::string fault_stage;
    double fault_at = 5;
    double fault_for = 2;
};

Options ParseOptions(const std::vector<std::string> &args) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto &arg = args[i];
        auto value = [&](std::string_view key) -> std::optional<std::string> {
            if (arg.starts_with(std::string(key) + "=")) return arg.substr(key.size() + 1);
            if (arg == key) {
                if (++i >= args.size()) throw std::runtime_error("Missing value: " + std::string(key));
                return args[i];
            }
            return std::nullopt;
        };
        if (arg == "--offline-video-test") continue;
        if (arg == "--performance") { options.input_probe = true; continue; }
        if (arg == "--performance-baseline") { options.performance_baseline = true; options.input_probe = true; continue; }
        if (arg == "--early-input") { options.early_input = true; options.input_placement = "early"; continue; }
        if (auto v = value("--capture")) options.capture = *v;
        else if (auto v = value("--report-dir")) options.report_dir = *v;
        else if (auto v = value("--seconds")) options.seconds = std::stod(*v);
        else if (auto v = value("--replay-window-seconds")) options.replay_window_seconds = std::stod(*v);
        else if (auto v = value("--fault-stage")) options.fault_stage = *v;
        else if (auto v = value("--fault-at")) options.fault_at = std::stod(*v);
        else if (auto v = value("--fault-for")) options.fault_for = std::stod(*v);
        else if (auto v = value("--ui-work-ms")) options.ui_work_ms = std::stod(*v);
        else if (auto v = value("--input-placement")) options.input_placement = *v;
        else if (arg == "--no-window-exercise") options.exercise_ui = false;
        else throw std::runtime_error("Unknown offline test option: " + arg);
    }
    if (options.capture.empty()) throw std::runtime_error("Use --capture=<network_capture.bin>; both recorded UDP and CustomByteBlock are required.");
    if (!std::isfinite(options.seconds) || options.seconds < 1 || options.seconds > 86400)
        throw std::runtime_error("--seconds must be between 1 and 86400");
    if (!std::isfinite(options.fault_at) || !std::isfinite(options.fault_for) || options.fault_at < 0 || options.fault_for < 0)
        throw std::runtime_error("Fault times must be finite and nonnegative");
    if (!std::isfinite(options.replay_window_seconds) || options.replay_window_seconds < 0 || options.replay_window_seconds > 86400)
        throw std::runtime_error("Replay window must be finite and nonnegative; zero means the full recording");
    if (!std::isfinite(options.ui_work_ms) || options.ui_work_ms < 0 || options.ui_work_ms > 50) throw std::runtime_error("--ui-work-ms must be between 0 and 50; it is an explicitly reported controlled CPU load");
    if (options.input_placement != "profile" && options.input_placement != "early" && options.input_placement != "late") throw std::runtime_error("--input-placement must be profile, early or late");
    if (options.early_input && !options.performance_baseline) throw std::runtime_error("--early-input is the sampling-only comparison and requires --performance-baseline");
    return options;
}

std::uint64_t Le(std::span<const std::uint8_t> bytes, std::size_t offset, std::size_t count) {
    if (offset > bytes.size() || count > bytes.size() - offset) throw std::runtime_error("Truncated capture integer");
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < count; ++i) value |= std::uint64_t(bytes[offset + i]) << (8 * i);
    return value;
}

std::uint32_t Crc32(std::span<const std::uint8_t> bytes) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ ((value & 1) ? 0xedb88320U : 0U);
            result[i] = value;
        }
        return result;
    }();
    std::uint32_t value = 0xffffffffU;
    for (auto byte : bytes) value = table[(value ^ byte) & 255] ^ (value >> 8);
    return value ^ 0xffffffffU;
}

std::vector<std::uint8_t> ExtractCustomBlock(std::span<const std::uint8_t> bytes) {
    // CustomByteBlock protobuf field 1 (bytes). No control message is parsed.
    if (bytes.empty() || bytes[0] != 0x0a) throw std::runtime_error("CustomByteBlock is missing protobuf field 1");
    std::size_t cursor = 1, length = 0;
    unsigned shift = 0;
    for (;;) {
        if (cursor >= bytes.size() || shift > 28) throw std::runtime_error("Bad CustomByteBlock length");
        const auto byte = bytes[cursor++];
        length |= std::size_t(byte & 127) << shift;
        if (!(byte & 128)) break;
        shift += 7;
    }
    if (length != 300 || cursor + length != bytes.size()) throw std::runtime_error("Expected exactly 300 bytes of CustomByteBlock data");
    return {bytes.begin() + static_cast<std::ptrdiff_t>(cursor), bytes.end()};
}

struct ReplayPacket {
    std::uint64_t time_us = 0;
    std::uint64_t source_index = 0;
    int lane = 0;
    std::vector<std::uint8_t> bytes;
};
struct Source {
    std::vector<ReplayPacket> packets;
    std::uint64_t records = 0, ignored_control_or_other = 0;
    std::array<std::uint64_t, 2> lane_packets{};
    std::uint64_t period_us = 0;
    std::array<std::uint64_t, 2> original_lane_packets{};
    std::uint64_t original_packets = 0, original_span_us = 0, selected_end_us = 0;
    std::string raw_sha256;
    bool partial_tail = false;
};

std::string Sha256File(const std::string &path) {
    auto input = platform_util::OpenBinaryInput(path);
    if (!input) throw std::runtime_error("Cannot open capture for SHA256");
    std::unique_ptr<AVSHA, void(*)(AVSHA*)> hash(av_sha_alloc(), [](AVSHA *value) { av_free(value); });
    if (!hash || av_sha_init(hash.get(), 256) != 0) throw std::runtime_error("Cannot initialize capture SHA256");
    std::array<std::uint8_t, 65536> buffer{};
    while (input) {
        input.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
        if (input.gcount() > 0) av_sha_update(hash.get(), buffer.data(), static_cast<std::size_t>(input.gcount()));
    }
    if (!input.eof()) throw std::runtime_error("Capture SHA256 read failed");
    std::array<std::uint8_t, 32> digest{}; av_sha_final(hash.get(), digest.data());
    std::ostringstream text; text << std::hex << std::setfill('0');
    for (auto byte : digest) text << std::setw(2) << unsigned(byte);
    return text.str();
}

Source LoadCapture(const std::string &path, double window_seconds) {
    const auto size = std::filesystem::file_size(platform_util::PathFromUtf8(path));
    if (size > 512ULL * 1024 * 1024) throw std::runtime_error("Capture exceeds the offline test's 512 MiB input limit");
    auto input = platform_util::OpenBinaryInput(path);
    if (!input) throw std::runtime_error("Cannot open recorded capture");
    std::array<std::uint8_t, 16> header{};
    input.read(reinterpret_cast<char *>(header.data()), header.size());
    if (input.gcount() != header.size() || std::memcmp(header.data(), "RBNCAP1\n", 8) || Le(header, 8, 4) != 2)
        throw std::runtime_error("Expected RBNCAP1 version 2 REC2 capture");
    Source source;
    for (;;) {
        std::array<std::uint8_t, 12> record{};
        input.read(reinterpret_cast<char *>(record.data()), record.size());
        if (input.gcount() == 0) break;
        if (input.gcount() != record.size()) { source.partial_tail = true; break; }
        if (std::memcmp(record.data(), "REC2", 4)) throw std::runtime_error("Capture record magic mismatch");
        const auto length = static_cast<std::size_t>(Le(record, 4, 4));
        if (length < 18 || length > 4U * 1024 * 1024) throw std::runtime_error("Invalid capture record length");
        std::vector<std::uint8_t> body(length);
        input.read(reinterpret_cast<char *>(body.data()), static_cast<std::streamsize>(body.size()));
        if (input.gcount() != body.size()) { source.partial_tail = true; break; }
        if (Crc32(body) != Le(record, 8, 4)) throw std::runtime_error("Capture CRC32 mismatch; no test was started");
        ++source.records;
        const auto channel_length = static_cast<std::size_t>(Le(body, 10, 2));
        const auto peer_length = static_cast<std::size_t>(Le(body, 12, 2));
        const auto payload_length = static_cast<std::size_t>(Le(body, 14, 4));
        if (18 + channel_length + peer_length + payload_length != body.size()) throw std::runtime_error("Capture field lengths mismatch");
        const std::string_view channel(reinterpret_cast<const char *>(body.data() + 18), channel_length);
        const auto payload = std::span(body).subspan(18 + channel_length + peer_length, payload_length);
        if (body[9] != 1) { ++source.ignored_control_or_other; continue; } // Only received data.
        ReplayPacket packet;
        packet.time_us = Le(body, 0, 8);
        if (body[8] == 2 && channel == "video-stream") {
            packet.lane = 0;
            packet.bytes.assign(payload.begin(), payload.end());
        } else if (body[8] == 1 && channel == "CustomByteBlock") {
            packet.lane = 1;
            packet.bytes = ExtractCustomBlock(payload);
        } else { ++source.ignored_control_or_other; continue; }
        ++source.lane_packets[packet.lane];
        source.packets.push_back(std::move(packet));
    }
    if (!source.lane_packets[0] || !source.lane_packets[1]) throw std::runtime_error("Capture must contain both real UDP HEVC shards and CustomByteBlock video");
    std::stable_sort(source.packets.begin(), source.packets.end(), [](const auto &a, const auto &b) { return a.time_us < b.time_us; });
    const auto first = source.packets.front().time_us;
    for (auto &packet : source.packets) packet.time_us -= first;
    source.original_packets = source.packets.size(); source.original_lane_packets = source.lane_packets;
    source.original_span_us = source.packets.back().time_us;
    source.selected_end_us = window_seconds > 0 ? static_cast<std::uint64_t>(window_seconds * 1'000'000) : source.original_span_us;
    if (window_seconds > 0) {
        if (source.selected_end_us > source.original_span_us) throw std::runtime_error("Replay window extends beyond the source recording");
        source.packets.erase(std::lower_bound(source.packets.begin(), source.packets.end(), source.selected_end_us,
            [](const ReplayPacket &packet, std::uint64_t end) { return packet.time_us < end; }), source.packets.end());
        source.lane_packets = {};
        for (const auto &packet : source.packets) ++source.lane_packets[packet.lane];
        if (!source.lane_packets[0] || !source.lane_packets[1]) throw std::runtime_error("Selected replay window must contain both real video lanes");
    }
    for (std::size_t i = 0; i < source.packets.size(); ++i) source.packets[i].source_index = i + 1;
    source.period_us = source.selected_end_us + 50'000; // Explicit 50ms tail; no extra packets are fabricated.
    source.raw_sha256 = Sha256File(path);
    return source;
}

struct QueuedPacket { const ReplayPacket *packet = nullptr; std::uint64_t loop = 0; Clock::time_point due, arrived; };
class PacketQueue {
public:
    static constexpr std::size_t capacity = 256;
    void Push(QueuedPacket packet) {
        { std::lock_guard lock(mutex_); if (queue_.size() == capacity) { queue_.pop_front(); ++dropped_; } queue_.push_back(packet); maximum_ = std::max(maximum_, queue_.size()); }
        cv_.notify_one();
    }
    bool Pop(QueuedPacket &packet, const std::atomic<bool> &stop) {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return stop.load() || !queue_.empty(); });
        if (stop.load()) return false;
        packet = queue_.front(); queue_.pop_front(); return true;
    }
    void Wake() { cv_.notify_all(); }
    std::array<std::uint64_t, 3> Stats() const { std::lock_guard lock(mutex_); return {queue_.size(), maximum_, dropped_}; }
private:
    mutable std::mutex mutex_; std::condition_variable cv_; std::deque<QueuedPacket> queue_;
    std::uint64_t dropped_ = 0; std::size_t maximum_ = 0;
};
struct Decoded { VideoFrame frame; std::uint64_t id = 0; Clock::time_point decoded_at; FrameTrace trace; };
struct Lane {
    PacketQueue input;
    LatestVideoFrameMailbox<Decoded> latest;
    std::atomic<std::uint64_t> inputs = 0, decoded = 0;
    std::atomic<int> width = 0, height = 0;
    std::atomic<std::uint64_t> trace_unknown = 0, conversion_allocations = 0, conversion_copied_bytes = 0;
    std::atomic<std::uint64_t> packet_allocations = 0, packet_reuses = 0, pool_busy = 0;
};
struct Shared {
    std::array<Lane, 2> lanes;
    performance::UdpArrivalRegistry udp_arrivals;
    bool baseline = false;
    std::atomic<bool> stop = false, failed = false;
    std::mutex stop_mutex, error_mutex; std::condition_variable stop_cv;
    std::string error;
    std::atomic<std::uint64_t> loops = 0, input_late_us_max = 0;
    void Stop() { stop = true; stop_cv.notify_all(); for (auto &lane : lanes) lane.input.Wake(); }
    void Fail(const std::string &text) { { std::lock_guard lock(error_mutex); error = text; } failed = true; Stop(); }
};

struct ThreadGroup {
    Shared &state;
    std::vector<std::thread> threads;
    explicit ThreadGroup(Shared &shared) : state(shared) { threads.reserve(3); }
    void Join() { state.Stop(); for (auto &thread : threads) if (thread.joinable()) thread.join(); }
    ~ThreadGroup() { Join(); }
};
struct BackendGuard {
    platform::PlatformBackend &backend;
    bool active = false;
    ~BackendGuard() { if (active) { try { backend.Shutdown(); } catch (...) {} } }
};

class TraceBook {
    std::map<std::int64_t, FrameTrace> pending_;
    std::int64_t next_ = 0;
public:
    void Clear() { pending_.clear(); }
    std::int64_t Add(DataArrival arrival, std::optional<std::int64_t> actual_pts = {}) {
        const auto token = actual_pts ? *actual_pts : ++next_;
        if (auto duplicate = pending_.find(token); duplicate != pending_.end()) {
            duplicate->second.arrival.known = false; // Reused/unresolved PTS is ambiguous, never overwrite a known source.
            return token;
        }
        FrameTrace trace; trace.token = token; trace.arrival = arrival; trace.decode_submit = Clock::now();
        pending_[token] = trace;
        if (pending_.size() > 4096) pending_.erase(pending_.begin());
        return token;
    }
    FrameTrace Take(std::int64_t pts) {
        const auto found = pending_.find(pts);
        if (found == pending_.end()) return {};
        auto trace = found->second; pending_.erase(found); return trace;
    }
};

void Publish(Lane &lane, VideoFrame frame, FrameTrace trace = {}) {
    lane.width = frame.width; lane.height = frame.height;
    if (!trace.arrival.known) ++lane.trace_unknown;
    Decoded output{std::move(frame), lane.decoded.fetch_add(1) + 1, Clock::now(), std::move(trace)};
    lane.latest.Push(std::move(output));
}
void DecodeUdp(Shared &state) {
    try {
        video::RgbaFrameConverter converter(state.baseline ? video::RgbaConversionMode::LegacyCopy : video::RgbaConversionMode::ReuseAligned);
        performance::CausalByteOrigins origins;
        TraceBook traces;
        bool ready = false;
        video::VideoDecoderConfig config;
        config.on_access_unit = [&](std::span<const std::uint8_t> bytes, std::int64_t offset, std::int64_t) {
            return traces.Add(origins.Resolve(bytes, offset));
        };
        video::VideoDecoder decoder([&](const AVFrame *frame) {
            auto trace = traces.Take(frame->pts);
            trace.decoded = Clock::now();
            auto converted = converter.Convert(frame);
            state.lanes[0].pool_busy = converter.PoolBusyCount();
            if (converted) {
                const auto sample = converter.LastSample();
                trace.converted = Clock::now(); trace.conversion_us = sample.convert_us; trace.copy_us = sample.copy_us;
                trace.conversion_total_us = sample.total_us; trace.copied_bytes = sample.copied_bytes; trace.conversion_allocation = sample.allocation;
                state.lanes[0].conversion_copied_bytes += sample.copied_bytes;
                if (sample.allocation) ++state.lanes[0].conversion_allocations;
                Publish(state.lanes[0], std::move(*converted), std::move(trace));
            }
        }, config);
        video::VideoShardsManager shards;
        std::uint64_t loop = std::numeric_limits<std::uint64_t>::max();
        shards.set_observed_frame_callback([&](const std::vector<std::uint8_t> &bytes, std::uint32_t id) {
            if (!ready) { const int nal = video::VideoDecoder::get_nal_type(bytes); ready = nal == 32 || nal == 33 || nal == 34 || nal == 19 || nal == 20 || nal == 21; }
            if (ready) {
                origins.Append(bytes, state.udp_arrivals.Resolve(loop, static_cast<std::uint16_t>(id), bytes));
                decoder.decode_frame(bytes);
                const auto stats = decoder.statistics();
                state.lanes[0].packet_allocations = stats.packet_allocations;
                state.lanes[0].packet_reuses = stats.packet_buffer_reuses;
            }
        });
        shards.set_reset_callback([&] { decoder.reset(); origins.Reset(); traces.Clear(); ready = false; });
        QueuedPacket item;
        while (state.lanes[0].input.Pop(item, state.stop)) {
            if (item.loop != loop) {
                if (loop != std::numeric_limits<std::uint64_t>::max()) { shards.reset_stream(); decoder.reset(); }
                origins.Reset(); traces.Clear(); ready = false; loop = item.loop;
            }
            state.lanes[0].inputs.fetch_add(1);
            shards.on_receive_shard(item.packet->bytes.data(), item.packet->bytes.size());
            shards.handle_once();
        }
    } catch (const std::exception &error) { state.Fail(std::string("UDP decode: ") + error.what()); }
      catch (...) { state.Fail("UDP decode: unknown exception"); }
}
void DecodeCustom(Shared &state) {
    try {
        performance::CausalByteOrigins origins(1024U * 1024U);
        TraceBook traces;
        DataArrival current_arrival;
        auto observer = std::make_shared<video::CustomByteBlockTraceObserver>();
        observer->on_payload = [&](std::span<const std::uint8_t> bytes) { origins.Append(bytes, current_arrival); };
        observer->on_access_unit = [&](std::int64_t pts, std::span<const std::uint8_t> bytes, std::int64_t offset) {
            traces.Add(origins.Resolve(bytes, offset), pts);
        };
        observer->on_reset = [&] { origins.Reset(); traces.Clear(); };
        video::CustomByteBlockVideoDecoderConfig config; config.trace_observer = observer;
        video::CustomByteBlockVideoDecoder decoder(config);
        std::uint64_t loop = std::numeric_limits<std::uint64_t>::max();
        QueuedPacket item;
        while (state.lanes[1].input.Pop(item, state.stop)) {
            if (item.loop != loop) { if (loop != std::numeric_limits<std::uint64_t>::max()) decoder.reset_stream(); origins.Reset(); traces.Clear(); loop = item.loop; }
            state.lanes[1].inputs.fetch_add(1);
            current_arrival = {true, item.loop, item.packet->source_index, item.packet->source_index, item.arrived, item.arrived};
            for (auto &frame : decoder.push_packet_bytes(item.packet->bytes)) {
                auto trace = traces.Take(frame.pts);
                trace.decoded = trace.converted = Clock::now(); // CBB combines decode/conversion; no invented split.
                Publish(state.lanes[1], {std::move(frame.rgba), frame.width, frame.height}, std::move(trace));
            }
        }
    } catch (const std::exception &error) { state.Fail(std::string("CustomByteBlock decode: ") + error.what()); }
      catch (...) { state.Fail("CustomByteBlock decode: unknown exception"); }
}
void Feed(const Source &source, Shared &state, Clock::time_point start) {
    try {
        for (std::uint64_t loop = 0; !state.stop; ++loop) {
            state.loops = loop + 1;
            for (const auto &packet : source.packets) {
                const auto due = start + std::chrono::microseconds(loop * source.period_us + packet.time_us);
                { std::unique_lock lock(state.stop_mutex); if (state.stop_cv.wait_until(lock, due, [&] { return state.stop.load(); })) return; }
                const auto late = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - due).count();
                auto old = state.input_late_us_max.load();
                while (late > 0 && std::uint64_t(late) > old && !state.input_late_us_max.compare_exchange_weak(old, static_cast<std::uint64_t>(late))) {}
                const auto arrived = Clock::now();
                if (packet.lane == 0) state.udp_arrivals.Add(loop, packet.source_index, packet.bytes, arrived);
                state.lanes[packet.lane].input.Push({&packet, loop, due, arrived});
            }
        }
    } catch (const std::exception &error) { state.Fail(std::string("Capture replay: ") + error.what()); }
      catch (...) { state.Fail("Capture replay: unknown exception"); }
}

struct ProcessSample { std::uint64_t private_bytes = 0, working_bytes = 0, handles = 0; };
ProcessSample ProcessStats() {
    ProcessSample result;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory))) {
        result.private_bytes = memory.PrivateUsage; result.working_bytes = memory.WorkingSetSize;
    }
    DWORD handles = 0; if (GetProcessHandleCount(GetCurrentProcess(), &handles)) result.handles = handles;
#endif
    return result;
}
std::string Quote(std::string_view text) {
    std::string result = "\"";
    for (char ch : text) { if (ch == '\\' || ch == '"') { result += '\\'; result += ch; } else if (ch == '\n') result += "\\n"; else if (ch == '\r') result += "\\r"; else if (static_cast<unsigned char>(ch) >= 32) result += ch; }
    return result + '"';
}
struct Presented { std::uint64_t id = 0; Clock::time_point decoded_at{}; int width = 0, height = 0; FrameTrace trace; Clock::time_point upload_begin{}, upload_end{}; };
double Age(const Presented &frame, Clock::time_point now) { return frame.id ? std::chrono::duration<double, std::milli>(now - frame.decoded_at).count() : -1; }

void DrawLane(platform::PlatformBackend &backend, TextureSlot slot, const char *name, double width, double age, std::uint64_t count) {
    ImGui::BeginChild(name, ImVec2(static_cast<float>(width), 0), ImGuiChildFlags_Borders);
    ImGui::Text("%s  presented=%llu  age since decode=%.0f ms", name, static_cast<unsigned long long>(count), age);
    const auto texture = backend.GetTextureView(slot);
    if (texture.IsReady()) {
        const auto available = ImGui::GetContentRegionAvail();
        const float scale = std::min(available.x / texture.size.x, available.y / texture.size.y);
        ImGui::Image(texture.texture_ref, ImVec2(texture.size.x * scale, texture.size.y * scale));
    } else ImGui::TextUnformatted("Waiting for a real decoded frame");
    ImGui::EndChild();
}
class LocalInputProbe {
    struct Event { std::uint64_t id; bool down; Clock::time_point enqueued; };
    RoboGo::testing::OfflineKeyboardMouseTestScope scope_;
    std::ostream &output_;
    Clock::time_point start_, next_{}, sampled_{};
    std::optional<Event> pending_;
    bool down_ = false, excluded_ = false;
    std::uint64_t generated_ = 0, matched_ = 0, mismatched_ = 0, packets_ = 0, scheduled_ = 0, skipped_ = 0;
public:
    LocalInputProbe(std::ostream &output, Clock::time_point start) : output_(output), start_(start), next_(start + std::chrono::seconds(2)) {
        scope_.Reset(); set_use_raw_mouse(true); enter_game_mode();
        output_ << "event_id,key_down,event_enqueued_us,sample_started_us,sink_received_us,event_to_sink_us,sample_to_sink_us,mouse_x,keyboard_value,matched,excluded\n";
    }
    void QueueIfDue() {
        const auto now = Clock::now();
        if (now < next_) return;
        const auto overdue = std::chrono::duration_cast<std::chrono::milliseconds>(now-next_).count();
        const auto slots = static_cast<std::uint64_t>(overdue/250) + 1;
        scheduled_ += slots; next_ += std::chrono::milliseconds(slots*250);
        if (pending_) { skipped_ += slots; return; }
        skipped_ += slots-1; // Keep fixed absolute slots; never inject a catch-up burst.
        down_ = (scheduled_ % 2) != 0;
        ImGui::GetIO().AddKeyEvent(ImGuiKey_W, down_);
        add_raw_mouse_delta(5.0f,0.0f); // 0.4 sensitivity -> actual serialized x=2; no platform mouse capture.
        pending_ = Event{++generated_,down_,Clock::now()};
    }
    void Sample(bool excluded) {
        excluded_ = excluded; sampled_ = Clock::now();
        update_keyboard_mouse_state(KeyboardMouseSink([&](const std::string &topic, const std::string &payload) {
            ++packets_;
            KeyboardMouseControl message;
            if (topic != "KeyboardMouseControl" || !message.ParseFromString(payload)) { ++mismatched_; return; }
            if (!pending_ || bool(message.keyboard_value() & 1U) != pending_->down) return;
            const auto received = Clock::now();
            const bool correct = message.keyboard_value() == (pending_->down ? 1U : 0U) && message.mouse_x() == 2 && message.mouse_y() == 0;
            if (correct) ++matched_; else ++mismatched_;
            const auto us = [&](Clock::time_point value) { return std::chrono::duration_cast<std::chrono::microseconds>(value-start_).count(); };
            output_ << pending_->id << ',' << pending_->down << ',' << us(pending_->enqueued) << ',' << us(sampled_) << ',' << us(received) << ','
                << std::chrono::duration<double,std::micro>(received-pending_->enqueued).count() << ',' << std::chrono::duration<double,std::micro>(received-sampled_).count() << ','
                << message.mouse_x() << ',' << message.keyboard_value() << ',' << correct << ',' << excluded_ << '\n';
            pending_.reset();
        }));
    }
    std::uint64_t Generated() const { return generated_; }
    std::uint64_t Matched() const { return matched_; }
    std::uint64_t Mismatched() const { return mismatched_; }
    std::uint64_t Packets() const { return packets_; }
    std::uint64_t Scheduled() const { return scheduled_; }
    std::uint64_t Skipped() const { return skipped_; }
};
} // namespace

int RunVideoStabilityTest(platform::PlatformBackend &backend, const std::vector<std::string> &args) {
    // No BackgroundServices, MQTT, sockets or VideoStreamManager (which binds UDP).
    // The optional local input scope only uses the no-network sink overload.
    try {
        const auto options = ParseOptions(args);
        const Source source = LoadCapture(options.capture, options.replay_window_seconds);
        auto report_dir = options.report_dir.empty()
            ? platform_util::ExecutableDirectory() / platform_util::PathFromUtf8("offline-video-test-" + std::to_string(std::time(nullptr)))
            : platform_util::PathFromUtf8(options.report_dir);
        std::filesystem::create_directories(report_dir);
        auto samples = platform_util::OpenUtf8Output(platform_util::PathToUtf8(report_dir / "samples.csv"));
        auto events = platform_util::OpenUtf8Output(platform_util::PathToUtf8(report_dir / "events.csv"));
        auto frames = platform_util::OpenUtf8Output(platform_util::PathToUtf8(report_dir / "frames.csv"));
        if (!frames) throw std::runtime_error("Cannot create per-frame performance trace");
        auto input_trace = platform_util::OpenUtf8Output(platform_util::PathToUtf8(report_dir / "input.csv"));
        if (!input_trace) throw std::runtime_error("Cannot create local input trace");
        frames << "profile,lane,output_id,token,associated,source_loop,first_packet,last_packet,first_ingress_us,complete_ingress_us,decode_submit_us,decoded_us,converted_us,upload_begin_us,upload_end_us,present_us,ingress_to_present_ms,decode_us,conversion_total_us,convert_us,copy_us,copied_bytes,conversion_allocation,mailbox_ms,upload_us,width,height,excluded,exclusion_reason\n";
        if (!samples || !events) throw std::runtime_error("Cannot create test report files");
        samples << "seconds,udp_decoded,cbb_decoded,udp_presented,cbb_presented,successful_presents,udp_decode_fps,cbb_decode_fps,udp_display_fps,cbb_display_fps,udp_age_ms,cbb_age_ms,udp_packet_queue,cbb_packet_queue,udp_packet_dropped,cbb_packet_dropped,udp_mailbox_pending,cbb_mailbox_pending,udp_overwritten,cbb_overwritten,private_bytes,working_bytes,handles,backend_uploads,backend_upload_failures,backend_busy,backend_recoveries,backend_resources,backend_wait_timeouts,backend_deferred,window_action,excluded,exclusion_reason,source_loop,udp_width,udp_height,cbb_width,cbb_height,successful_present_fps,udp_upload_fps,cbb_upload_fps,minimap_ready,minimap_width,minimap_height,minimap_presented\n";
        events << "seconds,event,detail\n";
        backend.EnableOfflineTestMode(true);
        BackendGuard backend_guard{backend};
        if (!options.fault_stage.empty() && options.fault_at == 0) backend.SetTestFailure(options.fault_stage, options.fault_for > 0 ? -1 : 1);
        if (!backend.Initialize()) { backend.Shutdown(); throw std::runtime_error("Offline backend Initialize failed; initialization faults cannot be counted as recovered"); }
        backend_guard.active = true;
        const auto mini_map_image = resource::LoadEmbeddedMapImage();
        if (!mini_map_image) throw std::runtime_error("Embedded production minimap did not decode; map coverage cannot be claimed");
        const bool initial_map_upload = backend.UploadTexture(TextureSlot::MiniMap, *mini_map_image);
        events << "0,minimap_initial_upload," << (initial_map_upload ? "accepted" : "cached_for_retry") << '\n';
        Shared state; state.baseline = options.performance_baseline;
        const auto start = Clock::now();
        std::unique_ptr<LocalInputProbe> input_probe;
        if (options.input_probe) input_probe = std::make_unique<LocalInputProbe>(input_trace,start);
        std::uint64_t controlled_work_checksum = 1;
        ThreadGroup workers(state);
        workers.threads.emplace_back([&] { DecodeUdp(state); });
        workers.threads.emplace_back([&] { DecodeCustom(state); });
        workers.threads.emplace_back([&] { Feed(source, state, start); });
        std::array<std::optional<Decoded>, 2> waiting_upload;
        std::array<Presented, 2> displayed{};
        std::array<std::uint64_t, 2> uploaded{}, presented{}, previous_decoded{}, previous_presented{};
        std::uint64_t actual_presents = 0, ui_iterations = 0, window_actions = 0, menu_exercises = 0;
        int window_action = -1;
        double last_sample_seconds = 0, elapsed = 0;
        double exclude_until = 2.0;
        std::string exclusion_reason = "startup";
        std::uint64_t observed_loop = 1, previous_presents = 0;
        std::array<std::uint64_t, 2> previous_uploaded{};
        std::uint64_t mini_map_presented = 0, map_presented_at_fault_clear = 0;
        bool mini_map_presented_once = false;
        ProcessSample first_memory = ProcessStats(), peak_memory = first_memory, last_memory = first_memory, warm_memory{};
        bool fault_started = options.fault_at == 0 && !options.fault_stage.empty(), fault_cleared = false, failure_observed = false;
        std::array<std::uint64_t, 2> at_fault_clear{};
        auto baseline_diagnostics = backend.GetRenderDiagnostics();
        std::string caught_error;
        try {
            while (!state.stop && backend.ProcessEvents()) {
                const auto now = Clock::now(); elapsed = std::chrono::duration<double>(now - start).count();
                if (elapsed >= options.seconds) break;
                if (state.loops.load() > observed_loop) { observed_loop = state.loops.load(); exclude_until = std::max(exclude_until, elapsed + 2); exclusion_reason = "loop_parameter_recovery"; }
                if (!options.fault_stage.empty() && !fault_started && elapsed >= options.fault_at) {
                    baseline_diagnostics = backend.GetRenderDiagnostics();
                    backend.SetTestFailure(options.fault_stage, options.fault_for > 0 ? -1 : 1); fault_started = true;
                    events << elapsed << ",fault_begin," << options.fault_stage << '\n';
                }
                if (fault_started && !fault_cleared && elapsed >= options.fault_at + std::max(1.0, options.fault_for)) {
                    backend.SetTestFailure("", 0); fault_cleared = true; at_fault_clear = presented;
                    map_presented_at_fault_clear = mini_map_presented;
                    events << elapsed << ",fault_clear," << options.fault_stage << '\n';
                    exclude_until = std::max(exclude_until, elapsed + 2); exclusion_reason = "fault_recovery";
                }
                const auto diagnostics = backend.GetRenderDiagnostics();
                if (fault_started && (diagnostics.upload_failures > baseline_diagnostics.upload_failures
                    || diagnostics.wait_timeouts > baseline_diagnostics.wait_timeouts
                    || diagnostics.recoveries > baseline_diagnostics.recoveries
                    || diagnostics.first_failure_stage.find(options.fault_stage) != std::string::npos)) failure_observed = true;
                if (options.exercise_ui) {
                    const double phase = std::fmod(elapsed, 60.0);
                    const int action = phase >= 15 && phase < 17 ? 2 : (phase >= 5 && phase < 10 ? 1 : 0);
                    if (action != window_action) { backend.RequestTestWindowAction(action); window_action = action; ++window_actions; exclude_until = std::max(exclude_until, elapsed + 2); exclusion_reason = "window_action_recovery"; events << elapsed << ",window_action," << action << '\n'; }
                }
                if (input_probe) input_probe->QueueIfDue();
                if (backend.BeginFrame()) {
                    ++ui_iterations;
                    const bool input_excluded = elapsed < exclude_until || window_action == 2 || (fault_started && !fault_cleared);
                    const bool sample_input_early = options.input_placement == "early" || (options.input_placement == "profile" && !state.baseline);
                    if (input_probe && sample_input_early) input_probe->Sample(input_excluded);
                    std::array<Presented, 2> pending_present{}; // Belongs only to this recording/submission.
                    for (int lane = 0; lane < 2; ++lane) {
                        if (auto latest = state.lanes[lane].latest.Take()) waiting_upload[lane] = std::move(latest);
                        if (waiting_upload[lane]) {
                            const auto upload_begin = Clock::now();
                            if (backend.UploadTexture(lane == 0 ? TextureSlot::UdpVideo : TextureSlot::CustomVideo, waiting_upload[lane]->frame)) {
                                pending_present[lane] = {waiting_upload[lane]->id, waiting_upload[lane]->decoded_at, waiting_upload[lane]->frame.width, waiting_upload[lane]->frame.height, waiting_upload[lane]->trace, upload_begin, Clock::now()};
                                ++uploaded[lane]; waiting_upload[lane].reset();
                            }
                        }
                    }
                    if (options.ui_work_ms > 0) {
                        const auto work_end = Clock::now() + std::chrono::duration<double,std::milli>(options.ui_work_ms);
                        while (Clock::now() < work_end) controlled_work_checksum = controlled_work_checksum * 1664525U + 1013904223U;
                    }
                    const auto *viewport = ImGui::GetMainViewport();
                    ImGui::SetNextWindowPos(viewport->WorkPos); ImGui::SetNextWindowSize(viewport->WorkSize);
                    ImGui::Begin("Offline video stability", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar);
                    if (ImGui::BeginMenuBar()) {
                        if (ImGui::Button("Test menu")) ImGui::OpenPopup("offline-actions");
                        if (options.exercise_ui && std::fmod(elapsed, 8.0) < 0.025) { ImGui::OpenPopup("offline-actions"); ++menu_exercises; }
                        if (ImGui::BeginPopup("offline-actions")) {
                            ImGui::TextUnformatted("Offline data only; no vehicle connection");
                            if (ImGui::MenuItem("Restore window")) backend.RequestTestWindowAction(0);
                            if (ImGui::MenuItem("Maximize window")) backend.RequestTestWindowAction(1);
                            if (ImGui::MenuItem("Clear injected failure")) backend.SetTestFailure("", 0);
                            ImGui::EndPopup();
                        }
                        ImGui::Text("%.1f / %.0f s   Present=%llu", elapsed, options.seconds, static_cast<unsigned long long>(actual_presents));
                        ImGui::EndMenuBar();
                    }
                    ImGui::TextUnformatted("REAL recorded UDP -> HEVC decode | REAL 300-byte CustomByteBlock -> H.264 decode");
                    ImGui::TextUnformatted("No network sockets, no keyboard/mouse publishing, no vehicle control.");
                    const auto mini_map = backend.GetTextureView(TextureSlot::MiniMap);
                    const bool mini_map_drawn = mini_map.IsReady();
                    if (mini_map_drawn) {
                        const float map_scale = 72.0f / std::max(mini_map.size.x, mini_map.size.y);
                        ImGui::Image(mini_map.texture_ref, ImVec2(mini_map.size.x * map_scale, mini_map.size.y * map_scale));
                        ImGui::SameLine();
                        ImGui::Text("Embedded production map  %.0fx%.0f  presented=%llu", mini_map.size.x, mini_map.size.y, static_cast<unsigned long long>(mini_map_presented));
                    } else ImGui::TextUnformatted("Waiting for embedded map / GPU recovery");
                    const float width = std::max(1.0f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2);
                    DrawLane(backend, TextureSlot::UdpVideo, "Official HEVC", width, Age(displayed[0], now), presented[0]); ImGui::SameLine();
                    DrawLane(backend, TextureSlot::CustomVideo, "CustomByteBlock H.264", width, Age(displayed[1], now), presented[1]);
                    ImGui::End();
                    if (input_probe && !sample_input_early) input_probe->Sample(input_excluded);
                    ImGui::Render();
                    if (backend.RenderFrame(ImGui::GetDrawData())) {
                        ++actual_presents;
                        if (mini_map_drawn) { ++mini_map_presented; mini_map_presented_once = true; }
                        for (int lane = 0; lane < 2; ++lane) if (pending_present[lane].id && pending_present[lane].id != displayed[lane].id) {
                            const auto submitted = Clock::now();
                            const auto &f = pending_present[lane]; const auto &t = f.trace;
                            const bool known = t.arrival.known && t.arrival.complete <= t.decode_submit && t.decode_submit <= t.decoded && t.decoded <= submitted;
                            const auto stamp = [&](Clock::time_point time) { return time == Clock::time_point{} ? -1LL : std::chrono::duration_cast<std::chrono::microseconds>(time - start).count(); };
                            const auto millis = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b-a).count(); };
                            const bool excluded_frame = elapsed < exclude_until || window_action == 2 || (fault_started && !fault_cleared);
                            const auto reason = window_action == 2 ? "minimized" : ((fault_started && !fault_cleared) ? "injected_failure" : (elapsed < exclude_until ? exclusion_reason.c_str() : "none"));
                            frames << (state.baseline ? "baseline" : "optimized") << ',' << (lane == 0 ? "udp" : "cbb") << ',' << f.id << ',' << t.token << ',' << known << ',' << t.arrival.loop << ',' << t.arrival.first_packet << ',' << t.arrival.last_packet << ','
                                << (known ? stamp(t.arrival.first) : -1) << ',' << (known ? stamp(t.arrival.complete) : -1) << ',' << stamp(t.decode_submit) << ',' << stamp(t.decoded) << ',' << stamp(t.converted) << ',' << stamp(f.upload_begin) << ',' << stamp(f.upload_end) << ',' << stamp(submitted) << ','
                                << (known ? millis(t.arrival.complete, submitted) : -1) << ',' << (known ? millis(t.decode_submit,t.decoded)*1000 : -1) << ',' << t.conversion_total_us << ',' << t.conversion_us << ',' << t.copy_us << ',' << t.copied_bytes << ',' << t.conversion_allocation << ',' << millis(t.converted, f.upload_begin) << ',' << millis(f.upload_begin,f.upload_end)*1000 << ','
                                << f.width << ',' << f.height << ',' << excluded_frame << ',' << reason << '\n';
                            displayed[lane] = pending_present[lane]; ++presented[lane]; pending_present[lane] = {};
                        }
                    }
                } else std::this_thread::yield(); // Backend owns bounded waits; do not add a second timer-granularity sleep.
                const auto sample_now = Clock::now();
                elapsed = std::chrono::duration<double>(sample_now - start).count();
                if (elapsed - last_sample_seconds >= 1.0) {
                    last_memory = ProcessStats();
                    peak_memory.private_bytes = std::max(peak_memory.private_bytes, last_memory.private_bytes);
                    peak_memory.working_bytes = std::max(peak_memory.working_bytes, last_memory.working_bytes);
                    peak_memory.handles = std::max(peak_memory.handles, last_memory.handles);
                    if (!warm_memory.private_bytes && elapsed >= 20) warm_memory = last_memory;
                    const auto q0 = state.lanes[0].input.Stats(), q1 = state.lanes[1].input.Stats();
                    const auto m0 = state.lanes[0].latest.Stats(), m1 = state.lanes[1].latest.Stats();
                    const auto d0 = state.lanes[0].decoded.load(), d1 = state.lanes[1].decoded.load();
                    const double span = elapsed - last_sample_seconds;
                    const auto gpu = backend.GetRenderDiagnostics();
                    const auto map_status = backend.GetTextureView(TextureSlot::MiniMap);
                    const bool fault_window = fault_started && !fault_cleared;
                    const bool excluded = elapsed < exclude_until || window_action == 2 || fault_window;
                    const std::string reason = window_action == 2 ? "minimized" : (fault_window ? "injected_failure" : (elapsed < exclude_until ? exclusion_reason : "none"));
                    samples << elapsed << ',' << d0 << ',' << d1 << ',' << presented[0] << ',' << presented[1] << ',' << actual_presents << ','
                        << (d0 - previous_decoded[0]) / span << ',' << (d1 - previous_decoded[1]) / span << ',' << (presented[0] - previous_presented[0]) / span << ',' << (presented[1] - previous_presented[1]) / span << ','
                        << Age(displayed[0], sample_now) << ',' << Age(displayed[1], sample_now) << ',' << q0[0] << ',' << q1[0] << ',' << q0[2] << ',' << q1[2] << ',' << m0.pending << ',' << m1.pending << ',' << m0.overwritten << ',' << m1.overwritten << ','
                        << last_memory.private_bytes << ',' << last_memory.working_bytes << ',' << last_memory.handles << ',' << gpu.uploads_succeeded << ',' << gpu.upload_failures << ',' << gpu.skipped_busy << ',' << gpu.recoveries << ',' << gpu.resource_creations << ',' << gpu.wait_timeouts << ',' << gpu.deferred_resources << ',' << window_action << ',' << excluded << ',' << reason << ',' << observed_loop << ',' << displayed[0].width << ',' << displayed[0].height << ',' << displayed[1].width << ',' << displayed[1].height << ',' << (actual_presents - previous_presents) / span << ',' << (uploaded[0] - previous_uploaded[0]) / span << ',' << (uploaded[1] - previous_uploaded[1]) / span << ',' << map_status.IsReady() << ',' << map_status.size.x << ',' << map_status.size.y << ',' << mini_map_presented << '\n';
                    samples.flush(); events.flush(); frames.flush(); input_trace.flush();
                    previous_decoded = {d0, d1}; previous_presented = presented; previous_uploaded = uploaded; previous_presents = actual_presents; last_sample_seconds = elapsed;
                    SPDLOG_INFO("[offline-video-test] seconds={:.1f} decoded={}/{} presented={}/{} private={} handles={}", elapsed, d0, d1, presented[0], presented[1], last_memory.private_bytes, last_memory.handles);
                }
            }
        } catch (const std::exception &error) { caught_error = error.what(); state.Fail(caught_error); }
          catch (...) { state.Fail("UI thread: unknown exception"); }
        workers.Join();
        elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        backend.SetTestFailure("", 0);
        const auto gpu = backend.GetRenderDiagnostics();
        last_memory = ProcessStats();
        const auto q0 = state.lanes[0].input.Stats(), q1 = state.lanes[1].input.Stats();
        const auto m0 = state.lanes[0].latest.Stats(), m1 = state.lanes[1].latest.Stats();
        const auto final_map = backend.GetTextureView(TextureSlot::MiniMap);
        const bool mini_map_ok = mini_map_presented_once && final_map.IsReady();
        const bool fault_recovered = mini_map_ok && (options.fault_stage.empty() || (fault_started && fault_cleared && failure_observed && presented[0] > at_fault_clear[0] && presented[1] > at_fault_clear[1] && mini_map_presented > map_presented_at_fault_clear));
        const bool completed = elapsed >= options.seconds && !state.failed;
        const bool smoke_valid = presented[0] > 0 && presented[1] > 0 && mini_map_ok;
        auto summary = platform_util::OpenUtf8Output(platform_util::PathToUtf8(report_dir / "summary.json"));
        summary << "{\n  \"offline_no_network\": true,\n  \"capture\": " << Quote(options.capture) << ",\n  \"requested_seconds\": " << options.seconds << ",\n  \"elapsed_seconds\": " << elapsed << ",\n  \"completed\": " << (completed ? "true" : "false") << ",\n  \"dual_stream_smoke_ok\": " << (smoke_valid ? "true" : "false") << ",\n  \"acceptance_evaluated\": false,\n  \"acceptance\": \"PENDING_STRICT_CSV_AUDIT\""
            << ",\n  \"performance_profile\": " << Quote(state.baseline ? "baseline" : "optimized")
            << ",\n  \"controlled_ui_work_ms\": " << options.ui_work_ms
            << ",\n  \"input_sampling_placement\": " << Quote(options.input_placement == "profile" ? (state.baseline ? "late" : "early") : options.input_placement)
            << ",\n  \"controlled_work_checksum\": " << controlled_work_checksum
            << ",\n  \"input_probe_enabled\": " << (options.input_probe ? "true" : "false")
            << ",\n  \"input_probe_definition\": \"Synthetic W and raw mouse event enqueue to actual UI sampling/serialization and local protobuf parse; no MQTT, network or vehicle. Same 13ms sampling gate.\""
            << ",\n  \"input_events_generated\": " << (input_probe ? input_probe->Generated() : 0)
            << ",\n  \"input_events_scheduled\": " << (input_probe ? input_probe->Scheduled() : 0)
            << ",\n  \"input_events_skipped_no_catchup\": " << (input_probe ? input_probe->Skipped() : 0)
            << ",\n  \"input_events_matched\": " << (input_probe ? input_probe->Matched() : 0)
            << ",\n  \"input_events_mismatched\": " << (input_probe ? input_probe->Mismatched() : 0)
            << ",\n  \"input_sink_packets\": " << (input_probe ? input_probe->Packets() : 0)
            << ",\n  \"performance_reference\": \"Same instrumented executable; baseline reproduces previous RGBA conversion and late input sampling. Historical 009 EXE is not being timed.\""
            << ",\n  \"latency_definition\": \"Actual ingress of all verified AU bytes to successful Present return; not photon or vehicle end-to-end latency. Unknown associations are never order-matched.\""
            << ",\n  \"trace_unknown_decoded\": [" << state.lanes[0].trace_unknown.load() << ',' << state.lanes[1].trace_unknown.load() << ']'
            << ",\n  \"udp_conversion_allocations\": " << state.lanes[0].conversion_allocations.load()
            << ",\n  \"udp_conversion_copied_bytes\": " << state.lanes[0].conversion_copied_bytes.load()
            << ",\n  \"udp_packet_allocations\": " << state.lanes[0].packet_allocations.load()
            << ",\n  \"udp_packet_reuses\": " << state.lanes[0].packet_reuses.load()
            << ",\n  \"udp_conversion_pool_busy\": " << state.lanes[0].pool_busy.load()
            << ",\n  \"raw_source_sha256\": " << Quote(source.raw_sha256) << ",\n  \"source_original_packets\": " << source.original_packets << ",\n  \"source_original_lane_packets\": [" << source.original_lane_packets[0] << ',' << source.original_lane_packets[1] << "],\n  \"source_original_span_us\": " << source.original_span_us << ",\n  \"replay_window_seconds\": " << options.replay_window_seconds << ",\n  \"selected_source_begin_us\": 0,\n  \"selected_source_end_us\": " << source.selected_end_us << ",\n  \"selected_source_packets\": " << source.packets.size() << ",\n  \"replay_tail_padding_us\": 50000,\n  \"replay_period_us\": " << source.period_us
            << ",\n  \"input_records\": " << source.records << ",\n  \"input_udp_packets\": " << source.lane_packets[0] << ",\n  \"input_cbb_packets\": " << source.lane_packets[1] << ",\n  \"ignored_control_and_other_records\": " << source.ignored_control_or_other << ",\n  \"capture_partial_tail\": " << (source.partial_tail ? "true" : "false")
            << ",\n  \"source_loops\": " << state.loops.load() << ",\n  \"input_lateness_max_us\": " << state.input_late_us_max.load() << ",\n  \"udp_decoded\": " << state.lanes[0].decoded.load() << ",\n  \"cbb_decoded\": " << state.lanes[1].decoded.load()
            << ",\n  \"udp_upload_accepted\": " << uploaded[0] << ",\n  \"cbb_upload_accepted\": " << uploaded[1] << ",\n  \"udp_new_presented\": " << presented[0] << ",\n  \"cbb_new_presented\": " << presented[1] << ",\n  \"successful_present_calls\": " << actual_presents << ",\n  \"ui_iterations\": " << ui_iterations
            << ",\n  \"udp_decoded_dimensions\": [" << state.lanes[0].width.load() << ',' << state.lanes[0].height.load() << "],\n  \"cbb_decoded_dimensions\": [" << state.lanes[1].width.load() << ',' << state.lanes[1].height.load() << "],\n  \"udp_displayed_dimensions\": [" << displayed[0].width << ',' << displayed[0].height << "],\n  \"cbb_displayed_dimensions\": [" << displayed[1].width << ',' << displayed[1].height << "],\n  \"udp_final_age_ms\": " << Age(displayed[0], Clock::now()) << ",\n  \"cbb_final_age_ms\": " << Age(displayed[1], Clock::now())
            << ",\n  \"minimap_required\": true,\n  \"minimap_source_dimensions\": [" << mini_map_image->width << ',' << mini_map_image->height << "],\n  \"minimap_initial_upload_accepted\": " << (initial_map_upload ? "true" : "false") << ",\n  \"minimap_ready\": " << (final_map.IsReady() ? "true" : "false") << ",\n  \"minimap_final_dimensions\": [" << final_map.size.x << ',' << final_map.size.y << "],\n  \"minimap_presented\": " << mini_map_presented << ",\n  \"minimap_presented_after_fault_clear\": " << (fault_cleared ? mini_map_presented - map_presented_at_fault_clear : 0)
            << ",\n  \"window_actions\": " << window_actions << ",\n  \"menu_exercises\": " << menu_exercises << ",\n  \"packet_queue_capacity_each\": 256,\n  \"packet_queue_max\": [" << q0[1] << ',' << q1[1] << "],\n  \"packet_dropped\": [" << q0[2] << ',' << q1[2] << "],\n  \"mailbox_capacity_each\": 1,\n  \"mailbox_pending_at_stop\": [" << m0.pending << ',' << m1.pending << "],\n  \"mailbox_overwritten\": [" << m0.overwritten << ',' << m1.overwritten << ']'
            << ",\n  \"private_bytes_start\": " << first_memory.private_bytes << ",\n  \"private_bytes_after_warmup\": " << warm_memory.private_bytes << ",\n  \"private_bytes_last\": " << last_memory.private_bytes << ",\n  \"private_bytes_peak\": " << peak_memory.private_bytes << ",\n  \"handles_start\": " << first_memory.handles << ",\n  \"handles_last\": " << last_memory.handles << ",\n  \"handles_peak\": " << peak_memory.handles
            << ",\n  \"backend_frames_submitted\": " << gpu.frames_submitted << ",\n  \"backend_uploads_succeeded\": " << gpu.uploads_succeeded << ",\n  \"backend_upload_failures\": " << gpu.upload_failures << ",\n  \"backend_recoveries\": " << gpu.recoveries << ",\n  \"backend_resource_creations\": " << gpu.resource_creations << ",\n  \"backend_deferred_max\": " << gpu.max_deferred_resources << ",\n  \"first_failure_stage\": " << Quote(gpu.first_failure_stage) << ",\n  \"first_hresult\": " << gpu.first_hresult
            << ",\n  \"fault_stage\": " << Quote(options.fault_stage) << ",\n  \"fault_observed\": " << (failure_observed ? "true" : "false") << ",\n  \"fault_cleared\": " << (fault_cleared ? "true" : "false") << ",\n  \"fault_recovered\": " << (fault_recovered ? "true" : "false") << ",\n  \"worker_error\": " << Quote(state.error)
            << ",\n  \"age_definition\": \"Time since the decoded frame currently committed by a successful Present; not end-to-end camera latency.\",\n  \"present_definition\": \"Only RenderFrame=true; accepted uploads and UI iterations are counted separately.\"\n}\n";
        summary.flush(); samples.flush(); events.flush(); frames.flush(); input_trace.flush();
        input_probe.reset();
        backend.RequestTestWindowAction(0); backend.Shutdown(); backend_guard.active = false;
        SPDLOG_INFO("[offline-video-test] finished completed={} dual_stream_smoke={} acceptance=PENDING_CSV_AUDIT report={}", completed, smoke_valid, platform_util::PathToUtf8(report_dir));
        return completed && smoke_valid && fault_recovered ? 0 : 2;
    } catch (const std::exception &error) {
        SPDLOG_ERROR("[offline-video-test] {}", error.what());
        return 1;
    } catch (...) {
        SPDLOG_ERROR("[offline-video-test] unknown exception"); return 1;
    }
}
} // namespace RoboGo::app
