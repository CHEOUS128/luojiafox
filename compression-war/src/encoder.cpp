#include "encoder.h"

#include "platform_util.h"
#include "raw_io.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace cw {

EncoderParams derive_encoder_params(const TransportSpec & transport,
                                    const StreamSpec & stream,
                                    bool gray,
                                    bool large_frame,
                                    bool single_packet,
                                    int bitrate_override_bps,
                                    bool full_range) {
    // ⚠ 与 autoaim io/video_sender/video_sender.cpp 的推导必须完全一致：
    //   kLargeFrameLeanFactor = 1.35，kSinglePacketBitrateRatio = 0.90
    //   85% 预算走 transport.budget_ratio（默认 0.85，与实车 yaml 相同）
    constexpr double kLargeFrameLeanFactor = 1.35;
    constexpr double kSinglePacketBitrateRatio = 0.90;
    constexpr std::size_t kMaxPayloadBytes = 296;
    const double kLargeFrameBudgetRatio = transport.budget_ratio;

    EncoderParams p;
    p.fps = stream.fps;
    p.gray = gray;
    p.large_frame = large_frame;
    p.single_packet = single_packet;
    p.gop = (large_frame || single_packet) ? 10 : 10;

    const std::size_t per =
      std::min<std::size_t>(transport.max_payload_bytes, kMaxPayloadBytes);
    const int max_pkts = std::clamp(transport.max_packets_per_frame, 1, 16);

    if (large_frame) {
        const double token_bps_d =
          transport.send_rate_hz * kLargeFrameBudgetRatio * static_cast<double>(kMaxPayloadBytes) *
          8.0 / kLargeFrameLeanFactor;
        const int token_bps = static_cast<int>(token_bps_d);
        // 默认与实车一致：yaml 写 0（自动）→ 用 token_bps。
        // 给了 --bitrate 就**硬覆盖**，不夹 min(用户值, token_bps)。
        //   理由：档案里「74kbps → 41.1 包/秒 = 102.8% 桶」那次实测就是高于 token_bps 跑的，
        //   当时那版还没有这个夹取。要复现那条曲线必须能真的把码率顶上去。
        p.bitrate_bps = (bitrate_override_bps <= 0) ? token_bps : bitrate_override_bps;
        p.vbv_bits = max_pkts * static_cast<int>(per) * 8;
    } else if (single_packet) {
        const std::size_t cap = std::min<std::size_t>(transport.max_payload_bytes, kMaxPayloadBytes);
        const int cap_bps = static_cast<int>(static_cast<double>(cap) * 8.0 * stream.fps *
                                             kSinglePacketBitrateRatio);
        p.bitrate_bps = (bitrate_override_bps <= 0) ? cap_bps : bitrate_override_bps;
        p.vbv_bits = std::max(p.bitrate_bps / 2, 1000);
    } else {
        // 旧的整包模式：按 yaml 的 bitrate_kbps 走，本仿真默认不用这条路
        p.bitrate_bps = (bitrate_override_bps <= 0) ? 90000 : bitrate_override_bps;
        p.vbv_bits = p.bitrate_bps / 2;
    }
    p.bitrate_bps = std::max(p.bitrate_bps, 1000);

    // ---- x264-params：逐条照抄 video_sender.cpp ----
    //   repeat-headers=1  → 每个关键帧前重复 SPS/PPS，客户端中途加入也能解
    //   annexb=1          → 输出带起始码的字节流
    //   ⚠ 这里**没有** aud=1（以前那行注释是错的）。实车 video_sender.cpp 不加 AUD，
    //     仿真也不加；切访问单元靠 first_mb_in_slice==0，字节数才能和实车对得上。
    std::string x264_params =
      "repeat-headers=1:annexb=1:bframes=0:scenecut=0:force-cfr=1:aq-mode=1:aq-strength=1.2";
    if (single_packet) {
        x264_params += ":keyint=1:min-keyint=1";
    }
    if (large_frame) {
        x264_params += ":keyint=" + std::to_string(p.gop) + ":min-keyint=" + std::to_string(p.gop);
        x264_params += ":slices=" + std::to_string(p.slices);
    }
    p.x264_params = x264_params;
    p.x264_preset = "slow";
    p.full_range = full_range;
    // ⚠ 别想着用 x264-params 的 ":range=pc" —— 在 ffmpeg 4.4.2 的 libx264 封装层里会被吞掉，
    //   实测 ffprobe 仍报 color_range=unknown。只有 ffmpeg 自己的 -color_range pc
    //   才会真正写进 SPS 的 VUI（video_full_range_flag=1）。见 build_ffmpeg_encode_args。
    return p;
}

std::vector<std::string> build_ffmpeg_encode_args(const std::string & raw_path,
                                                  const std::string & h264_path,
                                                  const SourceSpec & source,
                                                  const EncoderParams & params) {
    std::vector<std::string> a;
    a.push_back("-hide_banner");
    a.push_back("-loglevel");
    a.push_back("error");
    a.push_back("-nostdin");
    a.push_back("-y");

    // ---- 输入：裸帧 ----
    a.push_back("-f");
    a.push_back("rawvideo");
    a.push_back("-pix_fmt");
    a.push_back(params.gray ? "gray" : "yuv420p");
    a.push_back("-s");
    a.push_back(std::to_string(source.width) + "x" + std::to_string(source.height));
    a.push_back("-r");
    a.push_back(std::to_string(params.fps));
    // 写明亮度满量程（默认不开，实车 VideoSender 就是没开）。
    // 注意顺序：必须放在 -i 之前才被当成输入/输出侧的颜色属性。
    if (params.full_range) {
        a.push_back("-color_range");
        a.push_back("pc");
    } else if (params.gray) {
        // ★ 灰度输入必须**显式**钉成 TV range，不能靠默认值。
        //
        //   实测（同一条命令、同一份素材）：
        //     ffmpeg 4.4.2（沙盒）      → ffprobe 报 color_range=unknown
        //     ffmpeg 7.1.1（CH 机器）    → ffprobe 报 color_range=pc   ← 危险
        //
        //   灰度输入没有 chroma，范围本来是"未指定"，新版 ffmpeg/libx264 会按全量程
        //   写进 SPS 的 VUI；车端是 libx264 直接吃 GRAY8、什么标记都不写。
        //   一旦标了 pc，客户端解码器就不再按 limited 展开（1.16x−16），
        //   右栏会**比实车好看一大截**：2026-11-07 实测 CH 机器上 PSNR 38.78 / SSIM 0.9488，
        //   而车端口径是 PSNR≈29 / 色调 1.116x−15.5。这种"仿真比实车漂亮"的偏差
        //   会污染所有画质结论，所以必须在这里钉死。
        //
        //   显式写 tv 之后：4.4.2 输出仍是 unknown（SPS 逐字节不变，老基线不受影响），
        //   7.1.1 应写 limited。**判断有没有生效看 summary.txt 的"色调拟合"那一行**：
        //   `dec = 1.116 * src + -15.5` = 对；`dec ≈ 0.98 * src + 1.5` = 还是 pc。
        a.push_back("-color_range");
        a.push_back("tv");
    }
    a.push_back("-i");
    a.push_back(raw_path);

    // 只编前 N 帧。⚠ 必须是**输出侧**的 -frames:v。
    //   放在 -i 前面会变成输入侧选项，但 rawvideo 输入没有帧数可截，
    //   ffmpeg 会照读整个文件、正常编完，看起来"没生效"。
    if (source.max_frames > 0) {
        a.push_back("-frames:v");
        a.push_back(std::to_string(source.max_frames));
    }

    // ---- 编码 ----
    a.push_back("-c:v");
    a.push_back("libx264");
    a.push_back("-preset");
    a.push_back(params.x264_preset);
    a.push_back("-tune");
    a.push_back("zerolatency");
    a.push_back("-pix_fmt");
    a.push_back(params.gray ? "gray" : "yuv420p");
    a.push_back("-b:v");
    a.push_back(std::to_string(params.bitrate_bps));
    a.push_back("-minrate");
    a.push_back(std::to_string(params.bitrate_bps));
    a.push_back("-maxrate");
    a.push_back(std::to_string(params.bitrate_bps));
    a.push_back("-bufsize");
    a.push_back(std::to_string(params.vbv_bits));
    a.push_back("-g");
    a.push_back(std::to_string(params.gop));
    a.push_back("-keyint_min");
    a.push_back(std::to_string(params.gop));
    a.push_back("-bf");
    a.push_back("0");
    a.push_back("-x264-params");
    a.push_back(params.x264_params);
    // 只有显式要求时才钉线程数。不钉的时候 ffmpeg 按 CPU 数自己开，
    // 和实车 ctx->thread_count = 0 是同一个语义。
    if (params.threads > 0) {
        a.push_back("-threads");
        a.push_back(std::to_string(params.threads));
    }
    a.push_back("-fflags");
    a.push_back("+bitexact");
    a.push_back("-flags:v");
    a.push_back("+bitexact");
    a.push_back(h264_path);
    return a;
}

namespace {

// ⚠ 以前这里拼一整条命令行字符串再 std::system。Windows 上那样会踩两个坑：
//   1) system 走 cmd.exe，命令行按 ANSI(GBK) 解释 —— 中文路径直接崩
//   2) 自己拼引号很难和 CreateProcessW 的转义规则对齐
//   现在改成「参数表」形式，交给 platform_util::run_process 统一加引号。
//   这个函数只用来**打日志**，不参与实际执行。
std::string render_for_log(const std::string & exe, const std::vector<std::string> & args) {
    auto needs_quote = [](const std::string & s) {
        if (s.empty()) return true;
        return s.find_first_of(" \t\"") != std::string::npos;
    };
    auto wrap = [&](const std::string & s) {
        if (!needs_quote(s)) return s;
        std::string out = "\"";
        for (const char c : s) {
            if (c == '"') out += "\\\"";
            else out.push_back(c);
        }
        out += '"';
        return out;
    };
    std::string out = wrap(exe);
    for (const auto & a : args) {
        out += ' ';
        out += wrap(a);
    }
    return out;
}

}  // namespace

bool run_ffmpeg_encode(const std::string & ffmpeg_exe,
                       const std::string & raw_path,
                       const std::string & h264_path,
                       const SourceSpec & source,
                       const EncoderParams & params,
                       bool verbose,
                       std::string & error) {
    const auto args = build_ffmpeg_encode_args(raw_path, h264_path, source, params);

    if (verbose) {
        eout("[encode] %s\n", render_for_log(ffmpeg_exe, args).c_str());
    }

    const int rc = run_process(ffmpeg_exe, args);
    if (rc != 0) {
        error = "ffmpeg 编码失败（退出码 " + std::to_string(rc) + "）。命令：" +
                render_for_log(ffmpeg_exe, args);
        return false;
    }
    return true;
}

// ---- 编码线程时序 ----
//
// 逐条对应车端（video_sender.cpp:369-426）：
//
//   feed()   节拍按 interval **累加**，不是「now - last >= interval」：
//              last_send += interval;  if (last_send + interval < now) last_send = now + interval;
//            每放行一帧就 pending_frame = 这一帧（覆盖掉还在等的那帧）。
//   worker   空闲 → 立刻取走 pending_frame 并 reset()，然后编 encode_ms 毫秒。
//
// 仿真用**事件驱动**跑（不是按毫秒步进）：相机放行是精确的 k·Δ，
// 编码线程空闲/占用的切换也记成精确的 double 时刻，没有「凑整毫秒」的偏差。
// 只有两个事件会让状态变化：相机放行下一帧、编码线程编完一帧。
EncodeTimeline simulate_encode_timeline(std::size_t camera_frames,
                                        double fps,
                                        double encode_ms) {
    EncodeTimeline tl;
    tl.camera_frames = camera_frames;
    tl.au_camera.reserve(camera_frames);
    tl.au_done_ms.reserve(camera_frames);

    const double frame_interval_ms = 1000.0 / std::max(1e-6, fps);
    const double enc_ms = std::max(0.0, encode_ms);
    // --encode-ms 0：理论档，每帧都编，编完时刻 = 放行时刻（老基线就是这条）。
    if (enc_ms <= 1e-9) {
        for (std::size_t i = 0; i < camera_frames; ++i) {
            tl.au_camera.push_back(static_cast<std::uint32_t>(i));
            tl.au_done_ms.push_back(static_cast<double>(i) * frame_interval_ms);
        }
        return tl;
    }

    // 跑满「最后一帧放行 + 编完」即可；多给一个 interval 兜住浮点边界。
    const double last_release_ms =
      static_cast<double>(camera_frames > 0 ? camera_frames - 1 : 0) * frame_interval_ms;
    const double end_ms = last_release_ms + enc_ms + frame_interval_ms + 2.0;

    std::size_t next_frame = 0;
    double last_send_ms = 0.0;
    bool has_last_send = false;
    bool busy = false;
    double free_ms = 0.0;
    bool has_pending = false;
    std::uint32_t pending = 0;

    auto worker_take = [&](double now) {
        tl.au_camera.push_back(pending);
        tl.au_done_ms.push_back(now + enc_ms);
        busy = true;
        free_ms = now + enc_ms;
        has_pending = false;
    };

    // ⚠ 这里**不能**按 1 ms 步进：那样每次 encode_and_pack 返回都会被凑到下一个整毫秒，
    //   相当于悄悄把 encode_ms 抬高了约 1 ms，丢帧数就对不上车端了。
    //   车端本来就是「线程醒来立刻取帧、编完立刻再取」的连续时间，
    //   所以这里也只用精确 double 时刻推进：下一个事件 = min(相机放行, 编完)。
    double now = 0.0;
    while (true) {
        // ---- worker：编完 → 立刻回头取 pending（如果这期间相机放行过新帧）----
        if (busy && free_ms <= now + 1e-9) {
            now = free_ms;
            busy = false;
            if (has_pending) worker_take(now);
        }

        // ---- feed：按累加节拍放行下一帧 ----
        if (next_frame < camera_frames) {
            if (!has_last_send) {
                last_send_ms = now;
                has_last_send = true;
            }
            if (now + 1e-9 >= last_send_ms) {
                last_send_ms += frame_interval_ms;
                if (last_send_ms + frame_interval_ms < now) last_send_ms = now + frame_interval_ms;
                // pending_frame = 这一帧（覆盖掉还在等的旧帧），然后 notify worker
                pending = static_cast<std::uint32_t>(next_frame);
                has_pending = true;
                ++next_frame;
                if (!busy) worker_take(now);
            }
        } else if (!busy && !has_pending) {
            break;
        }

        // ---- 推进到下一个事件 ----
        double t_next = end_ms + 1.0;
        if (next_frame < camera_frames) t_next = last_send_ms;
        if (busy && free_ms < t_next) t_next = free_ms;
        if (t_next > end_ms + 1e-9) break;
        if (t_next <= now + 1e-12) t_next = now + 1e-9;   // 防死循环
        now = t_next;
    }
    return tl;
}

bool filter_encoder_input(const std::string & raw_path,
                          int width, int height, const std::string & pix_fmt,
                          const std::string & dst_path,
                          const std::vector<std::uint32_t> & camera_indices,
                          std::size_t camera_frames,
                          std::size_t & frames_written,
                          std::string & error) {
    frames_written = 0;
    const std::size_t frame_bytes = frame_bytes_for(pix_fmt, width, height);
    if (frame_bytes == 0) {
        error = "单帧字节数为 0（宽高或像素格式不对）";
        return false;
    }
    // 没有丢帧（存活帧就是全部相机帧）：原文件直接能用，别复制 1 GB
    if (camera_indices.size() == camera_frames) {
        frames_written = camera_indices.size();
        return true;
    }

    // 用平台层读整份，再按帧号切片写出。
    // ⚠ 不能 std::ifstream：Windows 上按 ANSI(GBK) 解释路径，中文目录必坏。
    std::vector<std::uint8_t> blob;
    if (!read_file(raw_path, blob, error)) return false;
    if (blob.size() % frame_bytes != 0) {
        error = "raw 文件大小不是单帧字节数的整数倍（尺寸或像素格式填错了）";
        return false;
    }
    const std::size_t total = blob.size() / frame_bytes;
    std::vector<std::uint8_t> out;
    out.reserve(camera_indices.size() * frame_bytes);
    for (const auto idx : camera_indices) {
        if (idx >= total) {
            error = "要取的帧号超出文件范围：" + std::to_string(idx) + " >= " + std::to_string(total);
            return false;
        }
        const auto * p = blob.data() + static_cast<std::size_t>(idx) * frame_bytes;
        out.insert(out.end(), p, p + frame_bytes);
    }
    if (!write_file(dst_path, out.data(), out.size(), error)) return false;
    frames_written = camera_indices.size();
    return true;
}

}  // namespace cw
