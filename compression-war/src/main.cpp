// main.cpp —— 压片仿真器命令行入口
//
// 最常用的一条：
//   cw_sim --input frames_192x152.gray --width 192 --height 152 --pix gray
//          --fps 20 --frames 300 --out runs/baseline
//
// 只给 --input 不给 --h264 时：自动调 ffmpeg 编码（实车档位参数自动推导）
// 给 --h264：跳过编码，直接包化这份码流（改包策略时用，省编码时间）
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "encoder.h"
#include "platform_util.h"
#include "raw_io.h"
#include "sim_config.h"
#include "sim_runner.h"

#ifndef CW_FFMPEG_EXE
#define CW_FFMPEG_EXE "ffmpeg"
#endif

namespace {

struct Args {
    std::string input;        // 编码器输入的裸帧（gray / yuv420p）
    std::string h264;         // 已有码流（可选）
    std::string out_dir = "runs/run1";
    std::string ffmpeg = CW_FFMPEG_EXE;
    std::string pix_fmt = "gray";
    int width = 192;
    int height = 152;
    double fps = 20.0;
    int max_frames = -1;

    // 播放器左栏用的「真原片」：同一取景、原分辨率、**没经过任何处理**。
    // 只影响显示，不参与编码/封包/统计。
    std::string ref_path;
    std::string ref_pix_fmt = "gray";   // 相机原生是 bgr24（彩色），不转灰度
    bool ref_is_png_dir = false;        // ref_path 是 PNG 帧目录（000000.png…）
    int ref_width = 1280;
    int ref_height = 1024;

    // 每帧编码耗时（毫秒）。实车 VideoSender 在独立线程里编码，这段是延迟的一部分。
    // 0 = 不算编码（默认；播放器会明确标注"未计"）。
    double encode_ms = 0.0;
    // 固定链路/客户端解码开销（毫秒），目前没有实测值，默认 0。
    double wire_extra_ms = 0.0;

    double send_rate_hz = 40.0;
    double budget_ratio = 0.85;
    int max_packets_per_frame = 8;
    int max_frame_bytes = 296;
    double drop_rate = 0.0;
    unsigned seed = 20261107u;

    bool large_frame = true;
    bool single_packet = false;
    bool gray = true;
    int gop = 10;
    int bitrate_bps = 0;      // 0 = 自动（与 yaml 的 video_bitrate_kbps: 0 一致）
    bool verbose = false;
    std::string viewer = "";  // 非空 = 导出左右对比播放器到该目录
    bool full_range = false;  // 在码流里写 full range 标记（实车不做，A/B 用）
    int threads = 0;          // 0 = 自动（实车）；1 = 确定性，对账/复现实验时用
    // 只算编码线程时序（哪些帧被丢、每帧什么时候编完），不读素材、不编码、不跑仿真。
    // 用途：拿一个真实日志的帧数对账，几秒出结果。
    bool timeline_only = false;
};

void print_usage() {
    cw::out(
      "用法：cw_sim --input <raw> [--h264 <stream>] --width W --height H --pix gray|yuv420p\n"
      "             --fps F --frames N --out <dir> [选项]\n"
      "\n"
      "必需：\n"
      "  --input <path>             编码器输入的裸帧（gray: W*H/帧；yuv420p: W*H*3/2/帧）\n"
      "  --width/--height           输入分辨率\n"
      "  --pix gray|yuv420p         输入像素格式（默认 gray）\n"
      "\n"
      "常用：\n"
      "  --h264 <path>              已有 Annex-B 码流；给了就不调 ffmpeg 编码\n"
      "  --fps <F>                  帧率（默认 20）\n"
      "  --frames <N>               只跑前 N 帧（默认全跑）\n"
      "  --out <dir>                输出目录（默认 runs/run1）\n"
      "  --ffmpeg <path>            ffmpeg 可执行文件（默认 PATH 里的 ffmpeg）\n"
      "\n"
      "传输：\n"
      "  --send-rate <Hz>           令牌桶速率（默认 40，协议上限 50）\n"
      "  --budget <0~1>             只用的比例（默认 0.85）\n"
      "  --max-pkts <N>             一帧最多拆几个包（默认 8）\n"
      "  --max-frame-bytes <B>      每包净载荷（默认 296）\n"
      "  --drop <p>                 随机丢包概率（默认 0）\n"
      "  --seed <N>                 随机种子\n"
      "\n"
      "编码：\n"
      "  --single-packet            单包模式（每帧独立 IDR，GOP=1）\n"
      "  --large-frame              大帧拆包（I+P GOP，默认开）\n"
      "  --color                    彩色（默认灰度）\n"
      "  --gop <N>                  GOP（默认 10）\n"
      "  --bitrate <bps>            目标码率；0=自动（默认，等于实车 yaml 的 0）\n"
      "  --full-range               码流里写 full range 标记（实车不做；用来量化那笔亮度损失）\n"
      "  --threads <N>              编码线程数；0=自动（默认，= 实车）。对账/复现请给 1\n"
      "  -v                         多打日志\n"
      "\n"
      "输出：\n"
      "  --viewer <dir>             左右对比播放器目录（默认 <out>/viewer，双击 player.html 看）\n"
      "  --no-viewer                不导出播放器\n"
      "  --ref <path>               播放器左栏用的真原片（同取景、原分辨率裸帧）\n"
      "  --original <pngdir>        播放器左栏 = 相机原片目录（000000.png…，原封不动）\n"
      "  --ref-size <WxH>           原片分辨率（默认 1280x1024）\n"
      "  --ref-pix gray|bgr24       原片像素格式（相机原生是 bgr24；默认 gray）\n"
      "\n"
      "延迟呈现（左=原片先出，右=处理完才出）：\n"
      "  --encode-ms <ms>           每帧编码耗时（实车 VideoSender 同款值；0=不计）\n"
      "                              > 1000/fps 时编码线程会丢帧，和车端 pending_frame 一致\n"
      "  --wire-ms <ms>             固定链路/解码开销（默认 0）\n"
      "\n"
      "对账：\n"
      "  --timeline-only            只算编码线程时序（丢几帧、每帧几时编完），不读素材\n"
      "                             配合 --frames/--fps/--encode-ms 用，几秒出结果\n");
}

std::string join_path(const std::string & dir, const std::string & name) {
    if (dir.empty()) return name;
    const char last = dir.back();
    if (last == '/' || last == '\\') return dir + name;
    return dir + "/" + name;
}

bool mkdir_p(const std::string & dir) {
    if (dir.empty()) return true;
    // Windows 上不能走 system("mkdir")：cmd.exe 按 ANSI(GBK) 解释，中文路径会错位
    std::string err;
    if (!cw::ensure_dir(dir, err)) {
        cw::eout("%s\n", err.c_str());
        return false;
    }
    return true;
}

bool parse_args(const std::vector<std::string> & args_in, Args & a, std::string & err) {
    for (std::size_t i = 0; i < args_in.size(); ++i) {
        const std::string & k = args_in[i];
        auto need = [&](const char * name) -> const char * {
            if (i + 1 >= args_in.size()) {
                err = std::string("参数 ") + name + " 缺少值";
                return nullptr;
            }
            return args_in[++i].c_str();
        };
        if (k == "--input") {
            const char * v = need("--input"); if (!v) return false; a.input = v;
        } else if (k == "--h264") {
            const char * v = need("--h264"); if (!v) return false; a.h264 = v;
        } else if (k == "--out") {
            const char * v = need("--out"); if (!v) return false; a.out_dir = v;
        } else if (k == "--ffmpeg") {
            const char * v = need("--ffmpeg"); if (!v) return false; a.ffmpeg = v;
        } else if (k == "--width") {
            const char * v = need("--width"); if (!v) return false; a.width = std::atoi(v);
        } else if (k == "--height") {
            const char * v = need("--height"); if (!v) return false; a.height = std::atoi(v);
        } else if (k == "--pix") {
            const char * v = need("--pix"); if (!v) return false; a.pix_fmt = v;
        } else if (k == "--fps") {
            const char * v = need("--fps"); if (!v) return false; a.fps = std::atof(v);
        } else if (k == "--frames") {
            const char * v = need("--frames"); if (!v) return false; a.max_frames = std::atoi(v);
        } else if (k == "--send-rate") {
            const char * v = need("--send-rate"); if (!v) return false; a.send_rate_hz = std::atof(v);
        } else if (k == "--budget") {
            const char * v = need("--budget"); if (!v) return false; a.budget_ratio = std::atof(v);
        } else if (k == "--max-pkts") {
            const char * v = need("--max-pkts"); if (!v) return false; a.max_packets_per_frame = std::atoi(v);
        } else if (k == "--max-frame-bytes") {
            const char * v = need("--max-frame-bytes"); if (!v) return false; a.max_frame_bytes = std::atoi(v);
        } else if (k == "--drop") {
            const char * v = need("--drop"); if (!v) return false; a.drop_rate = std::atof(v);
        } else if (k == "--seed") {
            const char * v = need("--seed"); if (!v) return false; a.seed = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
        } else if (k == "--gop") {
            const char * v = need("--gop"); if (!v) return false; a.gop = std::atoi(v);
        } else if (k == "--bitrate") {
            const char * v = need("--bitrate"); if (!v) return false; a.bitrate_bps = std::atoi(v);
        } else if (k == "--single-packet") {
            a.single_packet = true; a.large_frame = false;
        } else if (k == "--large-frame") {
            a.large_frame = true; a.single_packet = false;
        } else if (k == "--color") {
            a.gray = false;
        } else if (k == "--viewer") {
            const char * v = need("--viewer"); if (!v) return false; a.viewer = v;
        } else if (k == "--no-viewer") {
            a.viewer = "-";
        } else if (k == "--ref") {
            const char * v = need("--ref"); if (!v) return false; a.ref_path = v;
        } else if (k == "--original") {
            const char * v = need("--original"); if (!v) return false;
            a.ref_path = v; a.ref_is_png_dir = true;
        } else if (k == "--ref-pix") {
            const char * v = need("--ref-pix"); if (!v) return false; a.ref_pix_fmt = v;
        } else if (k == "--encode-ms") {
            const char * v = need("--encode-ms"); if (!v) return false; a.encode_ms = std::atof(v);
        } else if (k == "--wire-ms") {
            const char * v = need("--wire-ms"); if (!v) return false; a.wire_extra_ms = std::atof(v);
        } else if (k == "--ref-size") {
            const char * v = need("--ref-size"); if (!v) return false;
            // 接受 "1280x1024" / "1280X1024" / "1280*1024"
            const std::string s(v);
            const std::size_t sep = s.find_first_of("xX*");
            if (sep == std::string::npos) {
                err = "--ref-size 要写成 1280x1024 这种形式";
                return false;
            }
            a.ref_width = std::atoi(s.substr(0, sep).c_str());
            a.ref_height = std::atoi(s.c_str() + sep + 1);
            if (a.ref_width <= 0 || a.ref_height <= 0) {
                err = "--ref-size 的宽高必须为正";
                return false;
            }
        } else if (k == "--full-range") {
            a.full_range = true;
        } else if (k == "--threads") {
            const char * v = need("--threads"); if (!v) return false; a.threads = std::atoi(v);
        } else if (k == "--timeline-only") {
            a.timeline_only = true;
        } else if (k == "-v" || k == "--verbose") {
            a.verbose = true;
        } else if (k == "-h" || k == "--help") {
            print_usage();
            std::exit(0);
        } else {
            err = "不认识的参数: " + k;
            return false;
        }
    }
    if (a.timeline_only) {
        // 只算时序：不需要素材，--input 也不必须
        if (a.max_frames <= 0) {
            err = "--timeline-only 需要 --frames <N>（相机一共放行了多少帧）";
            return false;
        }
        if (a.fps <= 0.0) {
            err = "--fps 必须为正";
            return false;
        }
        if (a.encode_ms < 0.0) {
            err = "--encode-ms 不能是负数";
            return false;
        }
        return true;
    }
    if (a.input.empty() && a.h264.empty()) {
        err = "至少要给 --input 或 --h264";
        return false;
    }
    if (a.full_range && !a.h264.empty()) {
        err = "--full-range 是编码参数，不能和 --h264 一起用（码流已经编好了）";
        return false;
    }
    if (a.viewer == "-") a.viewer.clear();
    if (!a.gray) a.pix_fmt = "yuv420p";
    if (!a.gray && ((a.width & 1) || (a.height & 1))) {
        err = "yuv420p 下宽高必须是偶数";
        return false;
    }
    // --pix 也可能直接指定（例如给一份已经是 yuv420p 的输入）
    std::string norm_pix;
    if (!cw::normalize_pix_fmt(a.pix_fmt, norm_pix)) {
        err = "不支持的 --pix: " + a.pix_fmt + "（只认 gray / yuv420p / bgr24）";
        return false;
    }
    a.pix_fmt = norm_pix;
    std::string norm_ref_pix;
    if (!cw::normalize_pix_fmt(a.ref_pix_fmt, norm_ref_pix)) {
        err = "不支持的 --ref-pix: " + a.ref_pix_fmt + "（只认 gray / bgr24）";
        return false;
    }
    a.ref_pix_fmt = norm_ref_pix;
    if (a.encode_ms < 0.0 || a.wire_extra_ms < 0.0) {
        err = "--encode-ms / --wire-ms 不能是负数";
        return false;
    }
    if (a.encode_ms > 0.0 && a.encode_ms * a.fps / 1000.0 > 1.0) {
        // 一帧的编码时间超过帧间隔 → 车端编码线程跟不上 fps，pending_frame 会被新帧覆盖。
        // 这个仿真**会真的丢帧**（丢的帧不进 x264，和车端一致），不是只加延迟。
        // 这不是警告，是这一档配置的真实性质：那一轮的右栏必然会卡顿。
        cw::eout("[提示] --encode-ms %.0f ms × %.1f fps = %.2f > 1：编码线程跟不上相机节拍，"
                 "约 %.0f%% 的帧会在待编码时被覆盖丢掉（与车端 pending_frame 同语义）。\n",
                 a.encode_ms, a.fps, a.encode_ms * a.fps / 1000.0,
                 (1.0 - 1.0 / (a.encode_ms * a.fps / 1000.0)) * 100.0);
    }

    // ★ 比赛硬约束。协议 V2.0.0 的 0x0310：数据段 300 B、频率上限 50 Hz。
    //   超了就不是"参数调大了"，而是"这个配置在比赛里根本发不出去"——
    //   仿真跑出来的包率和画质全是在测一条不存在的链路，所以直接拒绝。
    //   留 50.0 本身是合法的（正好踩上限），只有 > 50 才拦。
    if (a.send_rate_hz <= 0.0 || a.send_rate_hz > cw::TransportSpec::protocol_max_send_rate_hz) {
        std::ostringstream m;
        m << "--send-rate 必须在 (0, " << cw::TransportSpec::protocol_max_send_rate_hz
          << "] Hz 之间（协议 0x0310 频率上限），现在是 " << a.send_rate_hz
          << "。仿真不允许跑出一条比赛里发不出去的链路。";
        err = m.str();
        return false;
    }
    if (a.max_frame_bytes <= 0 ||
        static_cast<std::size_t>(a.max_frame_bytes) >= cw::TransportSpec::protocol_packet_bytes) {
        std::ostringstream m;
        m << "--max-frame-bytes 必须小于 " << cw::TransportSpec::protocol_packet_bytes
          << " B（300 B 数据段减去 2B 长度 + 2B 序号 = 296 净载荷），现在是 "
          << a.max_frame_bytes;
        err = m.str();
        return false;
    }
    if (a.max_packets_per_frame < 1 || a.max_packets_per_frame > 16) {
        err = "--max-pkts 必须在 1~16 之间";
        return false;
    }
    return true;
}

// 写 summary.txt / per_frame.csv 用的公共出口：拼好整段再走平台层写盘
// （Windows 上 std::ofstream 内部是 fopen，中文路径会错位）
bool write_text_file(const std::string & path, const std::string & text) {
    std::string err;
    if (!cw::write_file(path, text.data(), text.size(), err)) {
        cw::eout("%s\n", err.c_str());
        return false;
    }
    return true;
}

void write_summary_txt(const std::string & path, const cw::RunSummary & s) {
    std::ostringstream f;
    f << "== 压片仿真结果 ==\n";
    f << "帧数(相机放行)      " << s.frames_camera << "\n";
    f << "帧数(编码线程丢)    " << s.frames_encode_dropped
      << "   ← 编码慢于 1/fps，pending_frame 被下一帧覆盖，这些帧从没进过 x264\n";
    f << "帧数(编码)          " << s.frames_encoded << "\n";
    f << "帧数(超限丢弃)      " << s.frames_oversize << "\n";
    f << "帧数(客户端解出)    " << s.frames_decoded << "\n";
    f << "帧数(进网未解出)    " << s.frames_lost << "\n";
    f << "包(发)              " << s.packets_emitted << "\n";
    f << "包(窗口外尾巴)      " << s.packets_tail << "\n";
    f << "包(收尾flush)       " << s.packets_flushed << "\n";
    f << "包(收)              " << s.packets_delivered << "\n";
    f << "包(积压丢弃)        " << s.packets_dropped_backlog << "\n";
    f << "包(模拟丢包)        " << s.packets_dropped_network << "\n";
    f << "包率                " << s.packets_per_second << " 包/秒（预算利用率 "
      << (s.budget_utilisation * 100.0) << "%）\n";
    // 档案里 35.5 / 41.1 两行大概率是这个口径（窗口内 + 收尾尾巴）。
    // 注意它只解释得动约 0.3 包/秒的差；剩下的大头在素材，见 README 第五节。
    f << "包率(含尾巴)        " << s.packets_per_second_incl_tail
      << " 包/秒   ← 档案口径；对不上时先怀疑素材，别怀疑链路\n";
    f << "需求包率            " << s.packets_demand_per_second << " 包/秒（"
      << (s.demand_utilisation * 100.0)
      << "%）  ← Σceil(帧字节/296)/时长；档案的 35.5/41.1 和 88.8%/102.8% 就是这个口径\n";
    f << "解码帧率            " << s.decode_fps << " fps\n";
    f << "帧完成延迟(中位)    " << s.frame_completion_ms_median
      << " ms   ← 拍照 → 最后一包发出，**已含编码**\n";
    f << "帧完成延迟(p95)     " << s.frame_completion_ms_p95 << " ms\n";
    f << "PSNR-Y 均值         " << s.psnr_y_mean << " dB\n";
    f << "SSIM-Y 均值         " << s.ssim_y_mean << "\n";
    f << "SSIM-Y 最小         " << s.ssim_y_min << "\n";
    f << "SSIM-Y(去色调)      " << s.ssim_y_tone_free_mean
      << "   ← 只反映压缩损失，不含 sws 范围约定错配造成的那部分\n";
    f << "色调拟合            dec = " << s.tone_k_median << " * src + " << s.tone_c_median
      << "   （1.16/-16 就是 limited range 展开）\n";
    f << "帧字节 中位/p95/max " << s.frame_bytes_median << " / " << s.frame_bytes_p95 << " / "
      << s.frame_bytes_max << "\n";
    f << "包/帧 均值          " << s.packets_per_frame_mean << "\n";

    write_text_file(path, f.str());
}

}  // namespace

int main(int argc, char ** argv) {
    // Windows 上先接管控制台：源码里的 UTF-8 中文走 WriteConsoleW，
    // 不管当前 chcp 是几都不会乱码；被重定向到文件时按 UTF-8 写。
    cw::init_console();

    // 参数也从宽字符命令行取：CMD 里传进来的中文路径是系统 ANSI 字节，
    // 直接读 argv 会得到 GBK 字符串，和内部 UTF-8 对不上。
    const std::vector<std::string> args = cw::collect_args(argc, argv);

    Args a;
    std::string err;
    if (!parse_args(args, a, err)) {
        cw::eout("参数错误：%s\n\n", err.c_str());
        print_usage();
        return 2;
    }
    if (!mkdir_p(a.out_dir)) {
        cw::eout("无法创建输出目录：%s\n", a.out_dir.c_str());
        return 2;
    }

    // --timeline-only：只算编码线程时序，不读素材、不编码、不跑仿真。
    // 用途：拿车上日志的「输入帧数 / 编码帧数」对账，几秒出结果。
    if (a.timeline_only) {
        const auto tl = cw::simulate_encode_timeline(
          static_cast<std::size_t>(a.max_frames), a.fps, a.encode_ms);
        const std::size_t cam = tl.camera_frames;
        const std::size_t enc = tl.au_camera.size();
        const double interval_ms = 1000.0 / std::max(1e-6, a.fps);
        cw::out("相机放行 %zu 帧，进 x264 %zu 帧，丢 %zu 帧（%.2f%%）\n",
                cam, enc, cam >= enc ? cam - enc : 0,
                cam ? 100.0 * static_cast<double>(cam - enc) / static_cast<double>(cam) : 0.0);
        if (!tl.au_done_ms.empty()) {
            std::vector<double> lag_ms;
            lag_ms.reserve(tl.au_done_ms.size());
            for (std::size_t j = 0; j < tl.au_done_ms.size(); ++j) {
                lag_ms.push_back(tl.au_done_ms[j] -
                                 static_cast<double>(tl.au_camera[j]) * interval_ms);
            }
            std::sort(lag_ms.begin(), lag_ms.end());
            cw::out("编码滞后 中位 %.1f ms，p95 %.1f ms，最大 %.1f ms\n",
                    lag_ms[lag_ms.size() / 2],
                    lag_ms[static_cast<std::size_t>(lag_ms.size() * 0.95)],
                    lag_ms.back());
            if (enc >= 2) {
                const double span_s = tl.au_done_ms.back() / 1000.0;
                cw::out("末帧编完 %.1f ms，编码线程实际出帧率 %.2f 帧/秒（相机 %.1f fps）\n",
                        tl.au_done_ms.back(), static_cast<double>(enc) / std::max(1e-6, span_s),
                        a.fps);
            }
        }
        return 0;
    }

    // 默认就把左右对比播放器导出来（CH 要的就是这个窗口），--no-viewer 才关
    if (a.viewer.empty()) a.viewer = join_path(a.out_dir, "viewer");

    // ffmpeg 默认值是个裸文件名（Windows 下 "ffmpeg.exe"）。
    // CreateProcessW 不会自己去 PATH 里找，先解析成绝对路径。
    // 顺序：当前目录 → exe 所在目录 → PATH。找不到就原样交给系统（顶多报个清楚的错）。
    {
        const std::string resolved = cw::find_executable(a.ffmpeg);
        if (!resolved.empty()) a.ffmpeg = resolved;
    }

    // --ref 给的是目录（里面有 000000.png）就按「相机原片目录」处理，
    // 不去猜扩展名；这样 CH 直接把 D:\cam_raw\cam_raw 丢进来就行。
    if (!a.ref_path.empty() && !a.ref_is_png_dir) {
        std::vector<std::uint8_t> probe;
        std::string probe_err;
        if (cw::read_file(a.ref_path + "/000000.png", probe, probe_err) && probe.size() > 8) {
            a.ref_is_png_dir = true;
            a.ref_width = 0;
            a.ref_height = 0;
        }
    }

    cw::SourceSpec src;
    src.raw_path = a.input;
    src.width = a.width;
    src.height = a.height;
    src.pix_fmt = a.pix_fmt;
    src.ref_path = a.ref_path;
    src.ref_pix_fmt = a.ref_pix_fmt;
    src.ref_is_png_dir = a.ref_is_png_dir;
    src.ref_width = (a.ref_path.empty() || a.ref_is_png_dir) ? 0 : a.ref_width;
    src.ref_height = (a.ref_path.empty() || a.ref_is_png_dir) ? 0 : a.ref_height;
    // --frames 同时限制「编码」和「读 raw」：以前只限制了读，ffmpeg 照样把整个
    // car1280.gray 编一遍，120 帧的素材也要等 200 帧的编码时间。
    src.max_frames = a.max_frames;

    cw::StreamSpec stream;
    stream.fps = a.fps;
    stream.h264_path = a.h264.empty() ? join_path(a.out_dir, "stream.h264") : a.h264;

    cw::TransportSpec tx;
    tx.send_rate_hz = a.send_rate_hz;
    tx.budget_ratio = a.budget_ratio;
    tx.max_packets_per_frame = a.max_packets_per_frame;
    tx.max_payload_bytes = static_cast<std::size_t>(a.max_frame_bytes);
    tx.drop_rate = a.drop_rate;
    tx.seed = a.seed;

    // ---- 编码线程时序：先算清"哪些帧真的进了 x264" ----
    //
    // ★ 这一步必须在编码**之前**做。车端编码线程只有一个 pending_frame，
    //   跟不上 fps 时旧帧会被新帧覆盖，那些帧从来没有进过 x264。
    //   所以只能把存活帧挑出来组成编码输入，而不是"全编完再把包丢掉"
    //   （那样 P 帧会引用车端不存在的参考帧，解码链会断）。
    //
    // --h264 直接喂现成码流时不需要算：码流里的访问单元就是当时编出来的那些帧。
    cw::EncodeTimeline timeline;
    if (a.h264.empty()) {
        // 先读一遍相机帧数：--frames 给了就用它，没给就按文件大小推。
        // 只取文件大小（stat / GetFileSizeEx），不读内容——1 GB 素材也是瞬间。
        std::size_t camera_frames = 0;
        if (a.max_frames > 0) {
            camera_frames = static_cast<std::size_t>(a.max_frames);
        } else {
            const std::size_t frame_bytes = cw::frame_bytes_for(a.pix_fmt, a.width, a.height);
            std::uint64_t file_bytes = 0;
            if (frame_bytes == 0) {
                cw::eout("宽高或像素格式非法，算不出单帧字节数\n");
                return 1;
            }
            if (!cw::file_size(a.input, file_bytes)) {
                cw::eout("读素材失败：%s\n", a.input.c_str());
                return 1;
            }
            camera_frames = static_cast<std::size_t>(file_bytes / frame_bytes);
        }
        timeline = cw::simulate_encode_timeline(camera_frames, a.fps, a.encode_ms);
        const std::size_t survivors = timeline.au_camera.size();
        const std::size_t dropped = camera_frames >= survivors ? camera_frames - survivors : 0;
        cw::eout("[编码线程] 相机放行 %zu 帧 → 进 x264 %zu 帧，丢 %zu 帧（%.1f%%）\n",
                 camera_frames, survivors, dropped,
                 camera_frames ? 100.0 * static_cast<double>(dropped) / static_cast<double>(camera_frames) : 0.0);
    }

    // ---- 编码（可选）----
    if (a.h264.empty()) {
        cw::EncoderParams ep =
          cw::derive_encoder_params(tx, stream, a.gray, a.large_frame, a.single_packet,
                                    a.bitrate_bps, a.full_range);
        ep.gop = a.gop;
        ep.threads = a.threads;
        const std::string threads_desc =
          ep.threads > 0 ? std::to_string(ep.threads) : std::string("auto(实车同款)");

        // 编码输入：只有**真的丢了帧**才需要写过滤副本。
        // 没丢帧时注意别去调 filter_encoder_input——它会按「存活帧=全部帧」提前返回、
        // 一个字节都不写，而路径却已经被换掉了。
        std::string encode_input = a.input;
        const std::size_t survivors = timeline.au_camera.size();
        const bool dropped_any = survivors != timeline.camera_frames;
        if (dropped_any) {
            const std::string filtered_path = join_path(a.out_dir, "encoder_input.raw");
            std::size_t written = 0;
            std::string filter_err;
            if (!cw::filter_encoder_input(a.input, a.width, a.height, a.pix_fmt, filtered_path,
                                          timeline.au_camera, timeline.camera_frames, written,
                                          filter_err)) {
                cw::eout("准备编码输入失败：%s\n", filter_err.c_str());
                return 1;
            }
            if (written != survivors) {
                cw::eout("准备编码输入失败：写出 %zu 帧，应为 %zu 帧\n", written, survivors);
                return 1;
            }
            encode_input = filtered_path;
            cw::eout("[encode] 编码输入已过滤：%zu 帧 → %s\n", survivors, encode_input.c_str());
        }
        cw::eout("[encode] %dx%d %s fps=%.1f bitrate=%d bps vbv=%d gop=%d preset=%s threads=%s\n",
             a.width, a.height, a.pix_fmt.c_str(), a.fps, ep.bitrate_bps, ep.vbv_bits,
             ep.gop, ep.x264_preset.c_str(), threads_desc.c_str());

        // 编码时不能再按 --frames 截断：过滤副本已经只含存活帧；
        // 没过滤时 surviviors 就是它要求的帧数。两种情况下都只编 survivors 帧。
        cw::SourceSpec enc_src = src;
        enc_src.max_frames = static_cast<int>(survivors);
        if (!cw::run_ffmpeg_encode(a.ffmpeg, encode_input, stream.h264_path, enc_src, ep,
                                   a.verbose, err)) {
            cw::eout("编码失败：%s\n", err.c_str());
            return 1;
        }
        // 仿真读 raw 时也读同一份输入，否则下标会和访问单元错位。
        src.raw_path = encode_input;
        src.max_frames = static_cast<int>(survivors);
    }

    cw::SimResult result;
    if (!cw::run_simulation(src.raw_path, stream.h264_path, src, stream, tx, a.verbose, a.out_dir,
                            result, err, a.viewer, a.encode_ms, a.wire_extra_ms, timeline)) {
        cw::eout("仿真失败：%s\n", err.c_str());
        return 1;
    }

    const std::string csv_path = join_path(a.out_dir, "per_frame.csv");
    const std::string txt_path = join_path(a.out_dir, "summary.txt");
    if (!write_text_file(csv_path, result.per_frame_csv)) return 1;
    write_summary_txt(txt_path, result.summary);

    const auto & s = result.summary;
    cw::out("\n");
    cw::out("  帧  相机 %llu 编码 %llu 解出 %llu（编码丢 %llu，超限 %llu，进网未解出 %llu）\n",
                (unsigned long long)s.frames_camera, (unsigned long long)s.frames_encoded,
                (unsigned long long)s.frames_decoded, (unsigned long long)s.frames_encode_dropped,
                (unsigned long long)s.frames_oversize, (unsigned long long)s.frames_lost);
    cw::out("  包  发 %llu 收 %llu（积压丢 %llu，模拟丢 %llu）  %.2f 包/秒 预算 %.1f%%\n",
                (unsigned long long)s.packets_emitted, (unsigned long long)s.packets_delivered,
                (unsigned long long)s.packets_dropped_backlog,
                (unsigned long long)s.packets_dropped_network, s.packets_per_second,
                s.budget_utilisation * 100.0);
    cw::out("      需求 %.2f 包/秒（%.1f%%）—— 档案的 35.5/41.1 是这个口径，可以超过 100%%\n",
                s.packets_demand_per_second, s.demand_utilisation * 100.0);
    if (s.packets_tail || s.packets_flushed) {
        // 「收」会大于「发」是正常的：窗口外尾巴和收尾 flush 的包也真的到网上了，
        // 只是不算进包率（分母只有传输窗口）。摊开写，不让读者去猜。
        cw::out("      另：窗口外尾巴 %llu + 收尾 flush %llu = %llu 包在窗口外发出（计入收，不计入包率）\n",
                    (unsigned long long)s.packets_tail, (unsigned long long)s.packets_flushed,
                    (unsigned long long)(s.packets_tail + s.packets_flushed));
        cw::out("      含尾巴包率 %.2f 包/秒（%.1f%%）—— 档案口径；剩余差异来自素材，见 README 第五节\n",
                    s.packets_per_second_incl_tail,
                    s.packets_per_second_incl_tail * 100.0 / std::max(1e-6, s.send_rate_hz));
    }
    cw::out("  帧字节 中位 %zu / p95 %zu / max %zu，%.3f 包/帧\n", s.frame_bytes_median,
                s.frame_bytes_p95, s.frame_bytes_max, s.packets_per_frame_mean);
    cw::out("  画质 PSNR-Y %.2f dB，SSIM-Y %.4f（min %.4f），去色调 SSIM %.4f\n",
                s.psnr_y_mean, s.ssim_y_mean, s.ssim_y_min, s.ssim_y_tone_free_mean);
    cw::out("      色调 dec = %.3f*src %+.1f（≈1.16/-16 即 limited 展开，这段损失与码率无关）\n",
                s.tone_k_median, s.tone_c_median);
    {
        // 这一行现在是**全链路**延迟：拍照 → 编码 → 拆包 / 令牌桶 → 最后一包发出。
        // （包是编完才存在的，所以编码时间已经含在里面，不再「另计」。）
        std::string enc_note = a.encode_ms > 0.0
          ? "含编码 " + std::to_string(static_cast<int>(a.encode_ms + 0.5)) + " ms/帧"
          : std::string("编码未计（给 --encode-ms 才算编码）");
        if (s.frames_encode_dropped > 0) {
            enc_note += "，编码线程丢 " + std::to_string(s.frames_encode_dropped) + " 帧";
        }
        cw::out("  延迟 中位 %.1f ms，p95 %.1f ms（拍照→最后一包发出；%s）\n",
                s.frame_completion_ms_median, s.frame_completion_ms_p95, enc_note.c_str());
    }
    // 合规角：规则天花板 vs 本次配置。两个数都要 ≤ 100%，否则这条链路比赛里发不出去。
    cw::out("  合规 规则上限 300 B × 50 Hz = 120 kbps（协议 0x0310）；本次配置 %zu B × %.0f Hz = %.0f kbps（%.0f%%）\n",
                static_cast<std::size_t>(a.max_frame_bytes), a.send_rate_hz,
                static_cast<double>(a.max_frame_bytes) * a.send_rate_hz * 8.0 / 1000.0,
                static_cast<double>(a.max_frame_bytes) * a.send_rate_hz * 8.0 * 100.0 /
                  cw::TransportSpec::protocol_ceiling_bps());
    cw::out("  输出 %s / %s\n\n", csv_path.c_str(), txt_path.c_str());
    return 0;
}
