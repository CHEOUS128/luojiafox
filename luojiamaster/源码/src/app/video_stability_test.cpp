#include "video_stability_test.h"

#include "app/latest_video_frame.h"
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

struct Options {
    std::string capture;
    std::string report_dir;
    double seconds = 1800;
    double replay_window_seconds = 0;
    bool exercise_ui = true;
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
        if (auto v = value("--capture")) options.capture = *v;
        else if (auto v = value("--report-dir")) options.report_dir = *v;
        else if (auto v = value("--seconds")) options.seconds = std::stod(*v);
        else if (auto v = value("--replay-window-seconds")) options.replay_window_seconds = std::stod(*v);
        else if (auto v = value("--fault-stage")) options.fault_stage = *v;
        else if (auto v = value("--fault-at")) options.fault_at = std::stod(*v);
        else if (auto v = value("--fault-for")) options.fault_for = std::stod(*v);
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
    source.period_us = source.selected_end_us + 50'000; // Explicit 50ms tail; no extra packets are fabricated.
    source.raw_sha256 = Sha256File(path);
    return source;
}

struct QueuedPacket { const ReplayPacket *packet = nullptr; std::uint64_t loop = 0; Clock::time_point due; };
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
struct Decoded { VideoFrame frame; std::uint64_t id = 0; Clock::time_point decoded_at; };
struct Lane {
    PacketQueue input;
    LatestVideoFrameMailbox<Decoded> latest;
    std::atomic<std::uint64_t> inputs = 0, decoded = 0;
    std::atomic<int> width = 0, height = 0;
};
struct Shared {
    std::array<Lane, 2> lanes;
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

class RgbaConverter {
public:
    ~RgbaConverter() { if (context_) sws_freeContext(context_); }
    std::optional<VideoFrame> Convert(const AVFrame *frame) {
        if (!frame || frame->width <= 0 || frame->height <= 0 || frame->width > 4096 || frame->height > 4096) return {};
        context_ = sws_getCachedContext(context_, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
            frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!context_) throw std::runtime_error("HEVC RGBA conversion context failed");
        VideoFrame output;
        output.width = frame->width; output.height = frame->height;
        output.buffer.resize(static_cast<std::size_t>(frame->width) * frame->height * 4);
        std::uint8_t *data[] = {output.buffer.data()}; int stride[] = {frame->width * 4};
        if (sws_scale(context_, frame->data, frame->linesize, 0, frame->height, data, stride) != frame->height)
            throw std::runtime_error("HEVC RGBA conversion returned incomplete rows");
        return output;
    }
private: SwsContext *context_ = nullptr;
};

void Publish(Lane &lane, VideoFrame frame) {
    lane.width = frame.width; lane.height = frame.height;
    Decoded output{std::move(frame), lane.decoded.fetch_add(1) + 1, Clock::now()};
    lane.latest.Push(std::move(output));
}
void DecodeUdp(Shared &state) {
    try {
        RgbaConverter converter;
        bool ready = false;
        video::VideoDecoder decoder([&](const AVFrame *frame) {
            if (auto converted = converter.Convert(frame)) Publish(state.lanes[0], std::move(*converted));
        });
        video::VideoShardsManager shards;
        shards.set_frame_callback([&](const std::vector<std::uint8_t> &bytes) {
            if (!ready) { const int nal = video::VideoDecoder::get_nal_type(bytes); ready = nal == 32 || nal == 33 || nal == 34 || nal == 19 || nal == 20 || nal == 21; }
            if (ready) decoder.decode_frame(bytes);
        });
        shards.set_reset_callback([&] { decoder.reset(); ready = false; });
        std::uint64_t loop = std::numeric_limits<std::uint64_t>::max();
        QueuedPacket item;
        while (state.lanes[0].input.Pop(item, state.stop)) {
            if (item.loop != loop) {
                if (loop != std::numeric_limits<std::uint64_t>::max()) { shards.reset_stream(); decoder.reset(); }
                ready = false; loop = item.loop;
            }
            state.lanes[0].inputs.fetch_add(1);
            shards.on_receive_shard(item.packet->bytes.data(), item.packet->bytes.size());
            shards.handle_once(); // Exactly one queued shard: this internal queue never builds up.
        }
    } catch (const std::exception &error) { state.Fail(std::string("UDP decode: ") + error.what()); }
      catch (...) { state.Fail("UDP decode: unknown exception"); }
}
void DecodeCustom(Shared &state) {
    try {
        video::CustomByteBlockVideoDecoder decoder;
        std::uint64_t loop = std::numeric_limits<std::uint64_t>::max();
        QueuedPacket item;
        while (state.lanes[1].input.Pop(item, state.stop)) {
            if (item.loop != loop) { if (loop != std::numeric_limits<std::uint64_t>::max()) decoder.reset_stream(); loop = item.loop; }
            state.lanes[1].inputs.fetch_add(1);
            for (auto &frame : decoder.push_packet_bytes(item.packet->bytes))
                Publish(state.lanes[1], {std::move(frame.rgba), frame.width, frame.height});
        }
        // No flush: stopping cannot invent an end-of-stream image from a partial final unit.
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
                state.lanes[packet.lane].input.Push({&packet, loop, due});
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
struct Presented { std::uint64_t id = 0; Clock::time_point decoded_at{}; int width = 0, height = 0; };
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
} // namespace

int RunVideoStabilityTest(platform::PlatformBackend &backend, const std::vector<std::string> &args) {
    // This function never constructs BackgroundServices, MQTT, sockets, input managers,
    // or a VideoStreamManager (its constructor would bind UDP).
    try {
        const auto options = ParseOptions(args);
        const Source source = LoadCapture(options.capture, options.replay_window_seconds);
        auto report_dir = options.report_dir.empty()
            ? platform_util::ExecutableDirectory() / platform_util::PathFromUtf8("offline-video-test-" + std::to_string(std::time(nullptr)))
            : platform_util::PathFromUtf8(options.report_dir);
        std::filesystem::create_directories(report_dir);
        auto samples = platform_util::OpenUtf8Output(platform_util::PathToUtf8(report_dir / "samples.csv"));
        auto events = platform_util::OpenUtf8Output(platform_util::PathToUtf8(report_dir / "events.csv"));
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
        Shared state;
        const auto start = Clock::now();
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
                if (backend.BeginFrame()) {
                    ++ui_iterations;
                    std::array<Presented, 2> pending_present{}; // Belongs only to this recording/submission.
                    for (int lane = 0; lane < 2; ++lane) {
                        if (auto latest = state.lanes[lane].latest.Take()) waiting_upload[lane] = std::move(latest);
                        if (waiting_upload[lane] && backend.UploadTexture(lane == 0 ? TextureSlot::UdpVideo : TextureSlot::CustomVideo, waiting_upload[lane]->frame)) {
                            pending_present[lane] = {waiting_upload[lane]->id, waiting_upload[lane]->decoded_at, waiting_upload[lane]->frame.width, waiting_upload[lane]->frame.height};
                            ++uploaded[lane]; waiting_upload[lane].reset();
                        }
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
                    ImGui::End(); ImGui::Render();
                    if (backend.RenderFrame(ImGui::GetDrawData())) {
                        ++actual_presents;
                        if (mini_map_drawn) { ++mini_map_presented; mini_map_presented_once = true; }
                        for (int lane = 0; lane < 2; ++lane) if (pending_present[lane].id && pending_present[lane].id != displayed[lane].id) {
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
                    samples.flush(); events.flush();
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
        summary.flush(); samples.flush(); events.flush();
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
