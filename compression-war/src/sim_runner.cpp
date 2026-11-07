#include "sim_runner.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <map>
#include <numeric>
#include <random>
#include <sstream>
#include <utility>

#include "metrics.h"
#include "encoder.h"
#include "packetizer.h"
#include "platform_util.h"
#include "raw_io.h"
#include "viewer.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "video/custom_byte_block_video_decoder.h"

namespace cw {

namespace {

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = p * static_cast<double>(v.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(idx));
    const auto hi = static_cast<std::size_t>(std::ceil(idx));
    if (lo == hi) return v[lo];
    const double t = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - t) + v[hi] * t;
}

std::size_t percentile_size(std::vector<std::size_t> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const double idx = p * static_cast<double>(v.size() - 1);
    return v[static_cast<std::size_t>(std::lround(idx))];
}

// 把 Y 平面存成 PNG（灰度图），方便肉眼比对。
// ⚠ 不直接用 stbi_write_png（内部 fopen 按 ANSI 解释路径，中文目录会写坏），
//   改成先编进内存再走平台层写文件。
void png_sink(void * context, void * data, int size) {
    auto * buf = static_cast<std::vector<std::uint8_t> *>(context);
    const auto * p = static_cast<const std::uint8_t *>(data);
    buf->insert(buf->end(), p, p + size);
}

void save_y_png(const std::string & path, const std::uint8_t * y, int w, int h) {
    std::vector<std::uint8_t> png;
    if (stbi_write_png_to_func(&png_sink, &png, w, h, 1, y, w) == 0) return;
    std::string err;
    if (!write_file(path, png.data(), png.size(), err)) {
        eout("[warn] 存对照图失败：%s\n", err.c_str());
    }
}

// CSV 的一行。逐帧画质是解码完才有的，所以先占位、解码后再回填。
struct CsvRow {
    std::uint32_t frame_index = 0;
    std::size_t frame_bytes = 0;
    std::size_t packets = 0;
    bool released = false;     // 这一帧的包有没有进网
    bool decoded = false;
    double psnr = -1.0;
    double ssim = -1.0;
    double completion_ms = -1.0;
    const char * why = "";     // 没解出来时，丢在哪一环（可查账用）
};

}  // namespace

bool run_simulation(const std::string & raw_path,
                    const std::string & h264_path,
                    const SourceSpec & source,
                    const StreamSpec & stream,
                    const TransportSpec & transport,
                    bool verbose,
                    const std::string & out_png_dir,
                    SimResult & result,
                    std::string & error,
                    const std::string & viewer_dir,
                    double encode_ms,
                    double wire_extra_ms,
                    const EncodeTimeline & timeline_in) {
    result = SimResult{};
    const bool gray = (source.pix_fmt == "gray");

    // ---- 1. 读编码器输入（"发送端看得见的那一帧"）----
    std::vector<RawFrame> raw_frames;
    if (!read_raw_frames(raw_path, source.width, source.height, source.pix_fmt, source.max_frames,
                         raw_frames, error)) {
        return false;
    }
    if (raw_frames.empty()) {
        error = "raw 文件里没有帧";
        return false;
    }

    // ---- 2. 读 H.264 并按 AUD 切帧 ----
    std::vector<AccessUnit> access_units;
    if (!split_access_units(h264_path, access_units, error)) {
        return false;
    }
    if (access_units.empty()) {
        error = "码流里没切出任何访问单元";
        return false;
    }

    // ---- 3. 编码线程时序：哪些帧进了 x264、每帧什么时候编完 ----
    //
    // ★ 真实链路：相机 → VideoSender::feed（按 1/fps 放行，pending_frame 只留最新一帧）
    //   → 编码线程取走（encode_ms）→ 编完立刻封包入队 → 令牌桶按 send_rate 放行。
    //   编码跟不上时，还没被取走的帧被下一帧覆盖，**从来没进过 x264**：
    //   所以 P 帧不会去参考它，解码链也不会断。
    //
    //   调用方（main.cpp）已经按这张表挑好了编码输入；这里的 raw_frames 就是
    //   「编码器真正看得见的那些帧」，与 access_units 一一对应。
    EncodeTimeline timeline = timeline_in;
    const std::size_t enc_frame_count = std::min(raw_frames.size(), access_units.size());
    if (raw_frames.size() != access_units.size()) {
        eout("[warn] 编码输入 %zu 帧 与访问单元 %zu 个 不一致，按较小值 %zu 跑\n",
             raw_frames.size(), access_units.size(), enc_frame_count);
    }
    if (timeline.au_camera.size() != enc_frame_count) {
        // 没给时序表（--h264 直接喂码流、或 sweep）：退回「每帧都编，编完=放行」。
        if (!timeline.au_camera.empty()) {
            eout("[warn] 编码时序表 %zu 项 与访问单元 %zu 个 不一致，改按逐帧编码处理\n",
                 timeline.au_camera.size(), enc_frame_count);
        }
        timeline = simulate_encode_timeline(enc_frame_count, stream.fps, 0.0);
    }
    const std::size_t camera_frames = timeline.camera_frames;
    // 相机帧号 → 访问单元下标（被编码线程丢掉的帧不在表里）
    std::vector<std::int64_t> camera_to_au(camera_frames, -1);
    for (std::size_t j = 0; j < timeline.au_camera.size(); ++j) {
        const auto c = timeline.au_camera[j];
        if (c < camera_frames) camera_to_au[c] = static_cast<std::int64_t>(j);
    }

    // ---- 4. 预封包：每个访问单元一编完就变成包（inner_seq 按编码顺序递增）----
    Packetizer packetizer(transport);
    TokenBucket bucket(transport);
    std::mt19937 rng(transport.seed);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);

    const double frame_interval_ms = 1000.0 / std::max(1e-6, stream.fps);
    // 传输窗口 = 相机放行的最后一帧之后那一拍。包率/预算利用率的分母就是它。
    const double window_end_ms = static_cast<double>(camera_frames) * frame_interval_ms;
    // 多跑 1 秒：让最后一个 I 帧攒够令牌自然发出，而不是靠 flush 硬塞；
    // 同时把「编码线程还在手里、编完要等令牌」的尾巴也真正走完。
    const double sim_end_ms = window_end_ms + 1000.0;

    std::vector<WirePacket> delivered;          // 按发送顺序
    std::vector<std::size_t> frame_bytes;       // 成功封包的帧的字节数
    std::uint64_t oversize_frames = 0;
    std::uint64_t frames_encoded = 0;
    std::map<std::uint32_t, double> first_packet_ms;
    std::map<std::uint32_t, double> last_packet_ms;

    struct PendingAu {
        std::uint32_t camera_index = 0;
        double done_ms = 0.0;                   // 编码完成的时刻（包从这一刻开始排队）
        std::vector<WirePacket> packets;
        bool enqueued = false;
    };
    std::vector<PendingAu> pending_aus;
    pending_aus.reserve(enc_frame_count);
    for (std::size_t j = 0; j < enc_frame_count; ++j) {
        PendingAu pa;
        pa.camera_index = timeline.au_camera[j];
        pa.done_ms = timeline.au_done_ms[j];
        if (packetizer.push_frame(access_units[j].bytes, pa.camera_index, pa.packets)) {
            ++frames_encoded;
            frame_bytes.push_back(access_units[j].bytes.size());
        } else {
            ++oversize_frames;
            pa.packets.clear();
        }
        pending_aus.push_back(std::move(pa));
    }
    // 时序表本身就是按编完时刻递增的（编码线程是串行的），这里再排一次只为稳妥
    std::sort(pending_aus.begin(), pending_aus.end(),
              [](const PendingAu & a, const PendingAu & b) { return a.done_ms < b.done_ms; });

    std::size_t next_au = 0;
    std::vector<WirePacket> out_batch;
    // 传输窗口结束时令牌桶已经放出的包数。窗口之后放出来的算「尾巴」（packets_tail），
    // 它们是真实发生的，但落在测量窗口外，不能算进包率。
    double packets_in_window = 0.0;

    // ---- 5. 虚拟时钟主循环：编完 → 入队 → 令牌桶 → 丢包 ----
    for (double now = 0.0; now <= sim_end_ms; now += 1.0) {
        // 编码线程编完一帧 → encode_and_pack 封包 → 整帧塞进待发队列
        while (next_au < pending_aus.size() && now + 1e-9 >= pending_aus[next_au].done_ms) {
            auto & pa = pending_aus[next_au];
            if (!pa.packets.empty()) bucket.enqueue_frame(std::move(pa.packets));
            pa.enqueued = true;
            ++next_au;
        }
        out_batch.clear();
        bucket.drain(now, stream.fps, out_batch);
        if (now <= window_end_ms + 1e-9) packets_in_window = bucket.packets_released();
        for (auto & p : out_batch) {
            const auto fid = p.frame_index;
            if (first_packet_ms.find(fid) == first_packet_ms.end()) first_packet_ms[fid] = now;
            last_packet_ms[fid] = now;
            if (transport.drop_rate <= 0.0 || uniform(rng) >= transport.drop_rate) {
                delivered.push_back(std::move(p));
            }
        }
    }

    // 尾部兜底：跑满时长后还有积压说明「码率 > 令牌桶能力」，这些包实车会一直排着直到被判积压，
    // 这里强行放出来只是为了让解码器能收尾；它们单独计数，不混进 packets_emitted，
    // 否则包率会被虚高到「发得比令牌还多」，而发不出去的真相就被抹掉了。
    out_batch.clear();
    bucket.flush(out_batch);
    const std::uint64_t flushed = static_cast<std::uint64_t>(out_batch.size());
    for (auto & p : out_batch) {
        const auto fid = p.frame_index;
        if (first_packet_ms.find(fid) == first_packet_ms.end()) first_packet_ms[fid] = sim_end_ms;
        last_packet_ms[fid] = sim_end_ms;
        if (transport.drop_rate <= 0.0 || uniform(rng) >= transport.drop_rate) {
            delivered.push_back(std::move(p));
        }
    }

    // ---- 4. 喂给客户端真实解码器（RoboGo::video::CustomByteBlockVideoDecoder）----
    RoboGo::video::CustomByteBlockVideoDecoder decoder;
    std::vector<std::uint32_t> released_ids;              // 进网的帧，按顺序
    std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> decoded_seq;  // (解码器frame_id, Y)
    std::uint64_t frames_decoded = 0;

    auto absorb = [&](std::vector<RoboGo::video::CustomByteBlockDecodedFrame> &&frames) {
        for (auto & f : frames) {
            std::vector<std::uint8_t> y(static_cast<std::size_t>(f.width) * f.height);
            rgba_to_y(f.rgba.data(), y.data(), f.width, f.height);
            decoded_seq.emplace_back(f.frame_id, std::move(y));
            ++frames_decoded;
        }
    };

    std::uint32_t last_fid = 0;
    bool have_fid = false;
    for (const auto & p : delivered) {
        if (!have_fid || p.frame_index != last_fid) {
            released_ids.push_back(p.frame_index);
            last_fid = p.frame_index;
            have_fid = true;
        }
        try {
            absorb(decoder.push_packet_bytes(std::span<const std::uint8_t>(p.bytes)));
        } catch (const std::exception & e) {
            if (verbose) eout("[warn] 解码器拒包: %s\n", e.what());
        }
    }
    try {
        absorb(decoder.flush());
    } catch (const std::exception & e) {
        if (verbose) eout("[warn] 解码器 flush 失败: %s\n", e.what());
    }

    // ---- 5. 帧号对齐 ----
    // 解码器的 frame_id 只数「真正送进 avcodec 的访问单元」。它丢包后会等下一个 I 帧，
    // 被跳过的帧不计数。而 released_ids 恰好也是「送进 avcodec 的顺序」，
    // 所以 解码器第 k 个输出帧 == released_ids[k]。两边对上之前不敢直接比像素。
    std::map<std::uint32_t, std::size_t> decoded_to_encoded;   // 解码frame_id → 编码帧号
    for (std::size_t k = 0; k < decoded_seq.size() && k < released_ids.size(); ++k) {
        decoded_to_encoded[decoded_seq[k].first] = released_ids[k];
    }

    std::map<std::uint32_t, const std::vector<std::uint8_t> *> decoded_y;
    for (const auto & kv : decoded_seq) decoded_y[kv.first] = &kv.second;

    // ---- 6. 逐帧画质 ----
    // ★ 逐帧表按**相机帧号**排（0…camera_frames-1），不是按编码帧号：
    //   编码线程丢掉的帧也要占一行，否则「左边有、右边没有」这件事在表里根本看不见。
    std::vector<CsvRow> rows(camera_frames);
    std::map<std::uint32_t, std::size_t> packets_per_frame;
    for (const auto & p : delivered) ++packets_per_frame[p.frame_index];

    std::vector<double> psnrs, ssims, ssims_tf, tone_ks, tone_cs;
    const std::size_t plane_bytes = static_cast<std::size_t>(source.width) * source.height;
    // 与 Packetizer::push_frame 里同口径，用来判「这一帧是不是超限被整帧丢了」
    const std::size_t per =
      std::min<std::size_t>(transport.max_payload_bytes, transport.max_payload());
    const std::size_t max_pkts =
      static_cast<std::size_t>(std::clamp(transport.max_packets_per_frame, 1, 16));

    for (std::size_t c = 0; c < camera_frames; ++c) {
        auto & row = rows[c];
        row.frame_index = static_cast<std::uint32_t>(c);
        const auto au = camera_to_au[c];
        if (au < 0) {
            // 编码线程没取走这一帧（pending_frame 被下一帧覆盖）——它在相机那儿存在，
            // 在发送端从未存在。左栏有、右栏永远等不到。
            row.why = "encode-drop";
            continue;
        }
        row.frame_bytes = access_units[static_cast<std::size_t>(au)].bytes.size();
        row.packets = packets_per_frame[static_cast<std::uint32_t>(c)];
        row.released = row.packets > 0;
        const auto fit = first_packet_ms.find(row.frame_index);
        const auto lit = last_packet_ms.find(row.frame_index);
        if (fit != first_packet_ms.end() && lit != last_packet_ms.end()) {
            row.completion_ms = lit->second - fit->second;
        }
        // 丢在哪一环：超限 → 没进网 → 进网了没解出
        const bool packetized = row.frame_bytes <= per * max_pkts;
        if (!packetized) row.why = "oversize";
        else if (!row.released) row.why = "backlog";
        else row.why = "inflight";
    }

    for (const auto & kv : decoded_to_encoded) {
        const std::size_t c = kv.second;
        if (c >= camera_frames) continue;
        const auto au = camera_to_au[c];
        if (au < 0) continue;
        const std::size_t e = static_cast<std::size_t>(au);
        if (e >= raw_frames.size()) continue;
        const auto dit = decoded_y.find(kv.first);
        if (dit == decoded_y.end()) continue;
        const auto * dec = dit->second;
        if (dec->size() != plane_bytes) continue;
        if (raw_frames[e].bytes.size() < plane_bytes) continue;    // yuv420p 时前 W*H 就是 Y
        auto & row = rows[c];
        row.decoded = true;
        row.why = "";
        row.psnr = psnr_y(raw_frames[e].bytes.data(), dec->data(), source.width, source.height);
        row.ssim = ssim_y(raw_frames[e].bytes.data(), dec->data(), source.width, source.height);
        if (row.psnr > 0) psnrs.push_back(row.psnr);
        if (row.ssim >= 0) ssims.push_back(row.ssim);
        if (row.ssim >= 0) {
            ssims_tf.push_back(
              ssim_y_tone_free(raw_frames[e].bytes.data(), dec->data(), source.width, source.height));
            double k = 1.0, c0 = 0.0;
            estimate_tone(raw_frames[e].bytes.data(), dec->data(), source.width, source.height, k, c0);
            tone_ks.push_back(k);
            tone_cs.push_back(c0);
        }
    }

    // 抽样存对照图（每 20 帧一对，最多 5 对）。按相机帧号取样，方便和左栏对齐。
    if (!out_png_dir.empty()) {
        std::size_t saved = 0;
        for (const auto & kv : decoded_to_encoded) {
            const std::size_t c = kv.second;
            if (c % 20 != 0 || c >= camera_frames) continue;
            const auto au = camera_to_au[c];
            if (au < 0) continue;
            const std::size_t e = static_cast<std::size_t>(au);
            const auto dit = decoded_y.find(kv.first);
            if (dit == decoded_y.end() || saved >= 5) continue;
            char a[512], b[512];
            std::snprintf(a, sizeof(a), "%s/frame_%04zu_src.png", out_png_dir.c_str(), c);
            std::snprintf(b, sizeof(b), "%s/frame_%04zu_dec.png", out_png_dir.c_str(), c);
            save_y_png(a, raw_frames[e].bytes.data(), source.width, source.height);
            save_y_png(b, dit->second->data(), source.width, source.height);
            ++saved;
        }
    }

    // ---- 7. 汇总 ----
    auto & s = result.summary;
    s.frames_camera = camera_frames;
    s.frames_encoded = frames_encoded;
    s.frames_encode_dropped = camera_frames >= timeline.au_camera.size()
                                ? camera_frames - timeline.au_camera.size()
                                : 0;
    s.frames_oversize = oversize_frames;
    s.packets_emitted = static_cast<std::uint64_t>(packets_in_window);
    s.packets_tail = static_cast<std::uint64_t>(bucket.packets_released()) - s.packets_emitted;
    s.packets_flushed = flushed;
    s.packets_delivered = delivered.size();
    s.packets_dropped_backlog = bucket.backlog_dropped();
    const std::uint64_t in_flight = s.packets_emitted + s.packets_tail + s.packets_flushed;
    const std::uint64_t lost_in_flight =
      in_flight >= delivered.size() + s.packets_dropped_backlog
        ? in_flight - delivered.size() - s.packets_dropped_backlog
        : 0;
    s.packets_dropped_network = lost_in_flight;
    s.frames_decoded = frames_decoded;

    // 「编码了但客户端一帧没解出来」= 真丢失。不要只数 released && !decoded：
    // 被积压守卫整帧丢掉的、被超限整帧丢掉的，同样是「发了却没到」，必须算进去。
    // ⚠ 编码线程丢掉的帧**不算**在这里：它们从来没进过发送端，是另一条账（frames_encode_dropped）。
    s.frames_lost = frames_encoded >= frames_decoded ? frames_encoded - frames_decoded : 0;

    // 包率的分母只算传输窗口，不算后面那 1 秒尾巴。
    // ⚠ 分母必须是**相机**帧数：编码丢帧不改变相机节拍，也不缩短这一轮素材的时长。
    const double duration_s =
      std::max(1e-6, static_cast<double>(camera_frames) / std::max(1e-6, stream.fps));
    s.packets_per_second = static_cast<double>(s.packets_emitted) / duration_s;
    // 含尾巴口径：档案里 35.5 / 41.1 两行是按「窗口内 + 尾巴」除以帧数时长算的，
    // 拿它对账；稳态看上一行。
    s.packets_per_second_incl_tail =
      static_cast<double>(s.packets_emitted + s.packets_tail) / duration_s;
    // 需求包率：这些帧"要"多少包，不是桶放了多少。档案的 35.5/41.1 就是这个口径。
    const std::size_t demand_per_packet =
      std::min<std::size_t>(transport.max_payload_bytes, transport.max_payload());
    std::uint64_t demand = 0;
    for (const auto b : frame_bytes) {
        demand += (b + demand_per_packet - 1) / demand_per_packet;
    }
    s.packets_demand_per_second = static_cast<double>(demand) / duration_s;
    s.demand_utilisation = s.packets_demand_per_second / std::max(1e-6, transport.send_rate_hz);
    // 和实车日志同口径：实车「88.8% 桶」= 实际包率 / send_rate_hz，
    // 不是 / (send_rate_hz × 0.85)。85% 的预算体现在编码码率那一侧。
    s.budget_utilisation = s.packets_per_second / std::max(1e-6, transport.send_rate_hz);
    s.send_rate_hz = transport.send_rate_hz;
    s.decode_fps = static_cast<double>(frames_decoded) / duration_s;

    // 延迟 = 这一帧"被相机拍到" → 它最后一个包发出的时刻。
    // ★ 现在这个数**已经包含编码**：包是编完才存在的，编码慢就收不到包，
    //   所以它和播放器右栏的延迟是同一个口径（再加 wire_extra_ms 就是右栏出现时刻）。
    std::vector<double> latency;
    for (const auto & kv : last_packet_ms) {
        const std::size_t c = kv.first;
        if (c >= camera_frames) continue;
        const double born_ms = static_cast<double>(c) * frame_interval_ms;
        latency.push_back(std::max(0.0, kv.second - born_ms));
    }
    s.frame_completion_ms_median = percentile(latency, 0.50);
    s.frame_completion_ms_p95 = percentile(latency, 0.95);

    if (!psnrs.empty()) {
        s.psnr_y_mean = std::accumulate(psnrs.begin(), psnrs.end(), 0.0) / psnrs.size();
    }
    if (!ssims.empty()) {
        s.ssim_y_mean = std::accumulate(ssims.begin(), ssims.end(), 0.0) / ssims.size();
        s.ssim_y_min = *std::min_element(ssims.begin(), ssims.end());
    }
    if (!ssims_tf.empty()) {
        s.ssim_y_tone_free_mean =
          std::accumulate(ssims_tf.begin(), ssims_tf.end(), 0.0) / ssims_tf.size();
    }
    if (!tone_ks.empty()) {
        s.tone_k_median = percentile(tone_ks, 0.50);
        s.tone_c_median = percentile(tone_cs, 0.50);
    }

    s.frame_bytes_median = percentile_size(frame_bytes, 0.50);
    s.frame_bytes_p95 = percentile_size(frame_bytes, 0.95);
    s.frame_bytes_max = frame_bytes.empty() ? 0 : *std::max_element(frame_bytes.begin(), frame_bytes.end());
    double pkts = 0.0;
    for (const auto & kv : packets_per_frame) pkts += static_cast<double>(kv.second);
    s.packets_per_frame_mean =
      packets_per_frame.empty() ? 0.0 : pkts / static_cast<double>(packets_per_frame.size());

    // ---- 8. 左右对比播放器 ----
    // 右侧按「这一帧最后一个包到齐的时刻」切换。这就是 CH 要的那个窗口：
    // 丢帧会表现为画面冻住，而不是悄悄少一帧。
    if (!viewer_dir.empty()) {
        ViewerSpec vs;
        vs.out_dir = viewer_dir;
        vs.fps = stream.fps;
        vs.frame_count = camera_frames;      // 左栏按相机帧数放，编码丢掉的帧在左边照样出现
        vs.width = source.width;
        vs.height = source.height;
        vs.gray = gray;
        // 编码耗时与固定链路开销：只影响播放器右栏"什么时候出现"，不影响包率/画质统计
        vs.encode_ms = std::max(0.0, encode_ms);
        vs.wire_extra_ms = std::max(0.0, wire_extra_ms);
        vs.encode_dropped = s.frames_encode_dropped;
        {
            // 码率按**实际编出来的那些帧**算（编码丢帧时不能拿全量帧数除）
            std::size_t total_bytes = 0;
            for (const auto & au : access_units) total_bytes += au.bytes.size();
            const double kbps = access_units.empty()
              ? 0.0
              : static_cast<double>(total_bytes) * 8.0 * stream.fps /
                  static_cast<double>(access_units.size()) / 1000.0;
            std::ostringstream sub;
            sub << source.width << "×" << source.height << "  " << (gray ? "灰度" : "彩色")
                << "  " << stream.fps << " fps  " << transport.send_rate_hz << " 包/秒"
                << "  码率 " << static_cast<int>(kbps + 0.5) << " kbps";
            sub << "  编码 " << (encode_ms > 0.0 ? std::to_string(static_cast<int>(encode_ms + 0.5)) + " ms/帧" : std::string("未计"));
            if (s.frames_encode_dropped > 0) {
                sub << "（跟不上，丢 " << s.frames_encode_dropped << " 帧）";
            }
            vs.subtitle = sub.str();
        }
        // 解码帧号 → 相机帧号（播放器左栏按相机帧号走）
        std::map<std::uint32_t, std::uint32_t> au_to_camera;
        for (std::size_t j = 0; j < timeline.au_camera.size(); ++j) {
            au_to_camera[static_cast<std::uint32_t>(j)] = timeline.au_camera[j];
        }
        std::vector<ViewerFrame> vf;
        vf.reserve(decoded_seq.size());
        for (const auto & kv : decoded_seq) {
            const auto it = decoded_to_encoded.find(kv.first);
            if (it == decoded_to_encoded.end()) continue;
            const auto cit = au_to_camera.find(static_cast<std::uint32_t>(it->second));
            if (cit == au_to_camera.end()) continue;
            ViewerFrame v;
            v.encoded_index = cit->second;
            v.width = source.width;
            v.height = source.height;
            v.y_plane = kv.second.data();
            // 相机拍到它的时刻：左边按名义帧率走，这就是时间基准
            v.capture_ms = static_cast<double>(v.encoded_index) * frame_interval_ms;
            const auto lit = last_packet_ms.find(v.encoded_index);
            v.arrive_ms = (lit == last_packet_ms.end()) ? 0.0 : lit->second;
            // ★ 右边必须等这一帧「处理完」才出现，而处理是**串行发生在时间轴上**的：
            //   编码线程拍完就开始编（占用 encode_ms，慢于 1/fps 时前一张会被覆盖）→
            //   编完立刻封包 → 令牌桶按 40 包/秒整帧放行 → 最后一包到齐。
            //   所以 last_packet_ms 里已经包含编码时间，这里**不能再加一次 encode_ms**，
            //   只补固定链路/解码开销（--wire-ms）。
            v.ready_ms = v.arrive_ms + std::max(0.0, wire_extra_ms);
            // 编码器吐出这一帧的时刻。和到齐时刻一起决定"右边什么时候能看见"——
            // 见 viewer.h 里 ViewerFrame::encode_done_ms 的说明（单一真值两份地方用）。
            const auto au_it = camera_to_au[v.encoded_index];
            if (au_it >= 0 && static_cast<std::size_t>(au_it) < timeline.au_done_ms.size()) {
                v.encode_done_ms = timeline.au_done_ms[static_cast<std::size_t>(au_it)];
            } else {
                v.encode_done_ms = v.arrive_ms;
            }
            vf.push_back(v);
        }
        // 解码顺序就是到达顺序，但保险起见按到齐时刻排一下
        std::sort(vf.begin(), vf.end(),
                  [](const ViewerFrame & a, const ViewerFrame & b) { return a.ready_ms < b.ready_ms; });
        // 左栏退回编码器输入时，索引也必须按**相机帧号**——中间被编码线程丢掉的帧
        // 没有编码器输入可显示，只能沿用上一张（这不影响右栏，纯粹是没给 --ref 时的兜底）。
        std::vector<const std::uint8_t *> src_planes(camera_frames, nullptr);
        {
            const std::uint8_t * last = nullptr;
            for (std::size_t c = 0; c < camera_frames; ++c) {
                const auto au = camera_to_au[c];
                if (au >= 0 && static_cast<std::size_t>(au) < raw_frames.size()) {
                    last = raw_frames[static_cast<std::size_t>(au)].bytes.data();
                }
                src_planes[c] = last;
            }
            if (src_planes.empty()) src_planes.push_back(nullptr);
        }

        // ---- 左侧"真原片"（可选）----
        // 只用于播放器显示，让左栏的分辨率远高于右栏，"糊"才看得见。
        // 读失败只是播放器左栏退回编码器输入，不中断仿真。
        std::vector<RawFrame> ref_frames;
        if (source.ref_is_png_dir) {
            // 相机原片目录：逐张 PNG 解码，**不做任何转换**
            std::string ref_err;
            if (read_png_frames(source.ref_path, static_cast<int>(camera_frames), ref_frames, ref_err)) {
                vs.ref_width = ref_frames[0].width;
                vs.ref_height = ref_frames[0].height;
                vs.ref_is_bgr = ref_frames[0].bgr;
                vs.ref_planes.reserve(ref_frames.size());
                for (const auto & f : ref_frames) vs.ref_planes.push_back(f.bytes.data());
                if (ref_frames.size() < camera_frames) {
                    eout("[warn] 原片只有 %zu 帧，少于相机的 %zu 帧，左栏后面的帧会缺\n",
                         ref_frames.size(), camera_frames);
                }
                eout("[viewer] 左栏 = 相机原片目录 %s（%d×%d，%s，原封不动）\n",
                     source.ref_path.c_str(), vs.ref_width, vs.ref_height,
                     vs.ref_is_bgr ? "彩色" : "灰度");
            } else {
                eout("[warn] 原片目录读不出来（%s），播放器左栏退回 %dx%d\n",
                     ref_err.c_str(), source.width, source.height);
            }
        } else if (!source.ref_path.empty() && source.ref_width > 0 && source.ref_height > 0) {
            std::string ref_err;
            // ★ 左栏要的是「相机原片，原封不动」：相机录下来是彩色 BGR 就原样显示，
            //   绝不在这里转灰度——转灰度是"我们的处理"，只属于右栏。
            //   旧的 1280x1024 灰度原片（car1280.gray）仍然能读，靠 ref_pix_fmt 区分。
            if (read_raw_frames(source.ref_path, source.ref_width, source.ref_height,
                                source.ref_pix_fmt, static_cast<int>(camera_frames), ref_frames,
                                ref_err)) {
                if (ref_frames.size() < camera_frames) {
                    eout("[warn] 原片只有 %zu 帧，少于相机的 %zu 帧，左栏后面的帧会缺\n",
                         ref_frames.size(), camera_frames);
                }
                vs.ref_width = source.ref_width;
                vs.ref_height = source.ref_height;
                vs.ref_is_bgr = (source.ref_pix_fmt == "bgr24" || source.ref_pix_fmt == "bgr");
                vs.ref_planes.reserve(ref_frames.size());
                for (const auto & f : ref_frames) vs.ref_planes.push_back(f.bytes.data());
            } else {
                eout("[warn] 原片读不出来（%s），播放器左栏退回 %dx%d\n",
                     ref_err.c_str(), source.width, source.height);
            }
        }

        if (!export_viewer(vs, src_planes, vf, error)) {
            eout("[warn] 对比播放器导出失败：%s\n", error.c_str());
        }
    }

    // ---- 9. 逐帧 CSV ----
    std::ostringstream csv;
    csv << "frame_index,frame_bytes,packets,released,decoded,psnr_y,ssim_y,completion_ms,why\n";
    for (const auto & r : rows) {
        csv << r.frame_index << ',' << r.frame_bytes << ',' << r.packets << ','
            << (r.released ? 1 : 0) << ',' << (r.decoded ? 1 : 0) << ',' << r.psnr << ',' << r.ssim
            << ',' << r.completion_ms << ',' << r.why << '\n';
    }
    result.per_frame_csv = csv.str();
    return true;
}

bool run_simulation_quiet(const std::string & raw_path,
                          const std::string & h264_path,
                          const SourceSpec & source,
                          const StreamSpec & stream,
                          const TransportSpec & transport,
                          RunSummary & summary,
                          std::string & error) {
    SimResult r;
    if (!run_simulation(raw_path, h264_path, source, stream, transport, false, "", r, error)) {
        return false;
    }
    summary = std::move(r.summary);
    return true;
}

}  // namespace cw
