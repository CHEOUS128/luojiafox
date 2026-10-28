#include "video_sender.hpp"

#include "tools/crc.hpp"
#include "tools/logger.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

namespace
{

// ---- 0x0310 帧固定字段 ----
constexpr std::uint8_t kFrameSOF = 0xA5;
constexpr std::uint16_t kCmdId = 0x0310;

// ---- 内层协议（与客户端 CustomByteBlockVideoDecoder 严格一致）----
// 0x0310 的 300 字节 data 字段内容：
//   [2B 内层长度 BE][内层包] ...（剩余字节补 0）
// 内层包：
//   [2B 序号 BE][H.264 数据]
constexpr std::size_t kTransportPacketBytes = 300;
constexpr std::size_t kTransportLengthPrefix = 2;
constexpr std::size_t kInnerSeqBytes = 2;
constexpr std::size_t kMaxInnerBytes = kTransportPacketBytes - kTransportLengthPrefix;  // 298
constexpr std::size_t kMaxPayloadBytes = kMaxInnerBytes - kInnerSeqBytes;               // 296

// 大帧模式的码率预算只占令牌桶的 85%（2026-10-28，CH 定的"包率用到上限 85%"）。
// 剩下 15% 是留给令牌桶抖动和实际通道波动的安全边距。
constexpr double kLargeFrameBudgetRatio = 0.85;
// 「码率 → 包率」的实测换算系数（2026-10-28，300 帧真实赛场画面量出来的）。
// 为什么不能用 296B × 包数 × 8 直接换算：每帧字节要向上取整到 296 的整数倍，
// 一帧 400B 也得占 2 个包（浪费 192B）。这个浪费比例随帧变大而变小：
//   60kbps：均值 371B/帧、1.777 包/帧 → 每包只装 208.9B → 系数 1.417
//   74kbps：均值 463B/帧 → 每包 224.9B → 系数 1.316
// 取 1.35 是折中。实际工作在 60kbps 时系数 1.417，意味着预算给 ≈59.6kbps 会落到
// 35.5 包/秒（令牌桶 40 的 88.8%），而不是名义上的 85%。
// 这个点靠令牌桶仿真确认过：60 秒 0 丢帧、最大积压 2 帧；
// 74kbps 实测 41.1 包/秒 = 102.8% 桶（已超），80kbps 仿真直接丢 110 包 / 17 帧。
// 宁可少给比特，也不要让令牌桶撑爆后丢整帧断参考链。
constexpr double kLargeFrameLeanFactor = 1.35;

// 发送节拍默认值：与 ACE 编码端 center_crop_packet_send_rate_hz=40 一致。
// 40Hz × 296B ≈ 94.7kbps 是这条通道的实际上限。
// 可通过 configs/standard3.yaml 的 video_send_rate_hz 覆盖，
// 低速率（如 2Hz）用于定位链路问题：每秒只发 2 个包，好数、好对比。
constexpr double kDefaultSendRateHz = 40.0;

void write_u16_le(std::uint8_t * out, std::uint16_t v)
{
  out[0] = static_cast<std::uint8_t>(v & 0xFF);
  out[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}

void write_u16_be(std::uint8_t * out, std::uint16_t v)
{
  out[0] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
  out[1] = static_cast<std::uint8_t>(v & 0xFF);
}

std::string ffmpeg_err(int errnum)
{
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buf{};
  av_strerror(errnum, buf.data(), buf.size());
  return std::string(buf.data());
}

}  // namespace

namespace io
{

struct VideoSender::Impl
{
  AVCodecContext * codec_ctx = nullptr;
  AVFrame * frame = nullptr;
  AVPacket * packet = nullptr;
  SwsContext * sws_ctx = nullptr;

  // ---- 仅工作线程访问 ----
  std::vector<std::uint8_t> stream_buf;   // 编码输出、尚未打包的字节
  std::size_t stream_offset = 0;          // 已消费到的位置
  std::size_t frame_cap = kMaxPayloadBytes;  // 单包模式下一帧的硬上限（字节）
  std::uint64_t stat_frames = 0;          // 单包模式统计：成功出包的帧数
  std::uint64_t stat_oversize = 0;        // 单包模式统计：超限被丢的帧数
  std::uint64_t stat_encode_fail = 0;     // 单包模式统计：编码器报错的次数
  std::size_t stat_last_bytes = 0;        // 最近一帧的字节数
  std::size_t stat_last_packets = 0;      // 最近一帧拆成了几个包（大帧模式）
  std::uint64_t stat_key_frames = 0;      // 大帧模式：成功发出的 I 帧数
  std::uint64_t stat_p_frames = 0;        // 大帧模式：成功发出的 P 帧数
  std::uint64_t stat_key_dropped = 0;     // 大帧模式：超限被丢的 I 帧数（最致命）
  std::uint64_t stat_oversize_logged = 0; // 已就该数报警过（同一份码率只提醒一次）
  std::uint64_t stat_pkt_lost = 0;        // 大帧模式：因整帧积压而被动丢掉的包数（含丢整帧）
  std::uint64_t stat_pkt_lost_logged = 0; // 已就该数报警过
  bool last_encoded_was_key = false;      // 上一个编码出来的帧是不是关键帧
  bool force_keyframe = false;            // 强制下一帧出 I 帧（I 帧丢太多时自救）
  int frames_since_key = 0;               // 距离上次成功发出 I 帧过了多少帧
  int gop = 1;                            // 生效的 GOP（I+P 模式），用于判断多久没 I 帧就自救
  std::chrono::steady_clock::time_point stat_last_log{};
  bool has_stat_last_log = false;

  // ---- 主线程与工作线程共享 ----
  std::mutex mtx;
  std::condition_variable cv;
  std::optional<cv::Mat> pending_frame;   // 待编码帧（只保留最新一帧，慢于 fps 时自动丢旧帧）
  bool encoding = false;                  // 工作线程是否正在编码（flush 需要等它结束）
  std::deque<std::vector<std::uint8_t>> pending;  // 已封好、等待节拍放出的 0x0310 帧
  std::deque<std::size_t> pending_frames;         // 大帧模式：每项是「一帧拆成几个包」
  bool quit = false;
  std::thread worker;
  std::uint16_t inner_seq = 0;            // 内层包序号（跨帧递增）
  std::uint8_t outer_seq = 0;             // 0x0310 帧序号
  std::int64_t next_pts = 0;

  // 40Hz 出包节拍：令牌桶。每过一个节拍积 1 个令牌，最多攒 4 个（≈100ms 的突发）。
  // 注意不能用"按调用补 8 个"的写法：主循环一帧调用一次，那样一帧就把包全发出去了，
  // 实际速率会远超 40Hz（实测仿真 80 包/秒），码率统计和串口压力都会失真。
  std::chrono::steady_clock::time_point last_tick{};
  bool has_last_tick = false;
  double tokens = 0.0;
  std::chrono::steady_clock::time_point last_drop_log{};

  std::chrono::steady_clock::time_point last_send{};
  bool has_last_send = false;

  ~Impl()
  {
    if (packet) av_packet_free(&packet);
    if (frame) av_frame_free(&frame);
    if (codec_ctx) avcodec_free_context(&codec_ctx);
    if (sws_ctx) sws_freeContext(sws_ctx);
  }
};

VideoSender::VideoSender(const VideoSenderConfig & config)
: impl_(std::make_unique<Impl>()), config_(config)
{
  if (config_.width <= 0 || config_.height <= 0) {
    throw std::invalid_argument("VideoSender: width/height must be positive");
  }
  if (config_.fps <= 0.0) {
    throw std::invalid_argument("VideoSender: fps must be positive");
  }
  if (config_.crop_size <= 0) {
    throw std::invalid_argument("VideoSender: crop_size must be positive");
  }
  // 两种模式互斥：single_packet 要求每帧都是 IDR，large_frame 要用 P 帧，同时开必然冲突
  if (config_.single_packet && config_.large_frame) {
    throw std::invalid_argument(
      "VideoSender: video_single_packet 和 video_large_frame 不能同时为 true"
      "（前者每帧 IDR+单包，后者 I+P+拆包）。回退请把 video_large_frame 改回 false。");
  }

  const AVCodec * codec = avcodec_find_encoder_by_name("libx264");
  if (codec == nullptr) {
    codec = avcodec_find_encoder(AV_CODEC_ID_H264);
  }
  if (codec == nullptr) {
    throw std::runtime_error("VideoSender: no H.264 encoder available (need libx264)");
  }

  impl_->codec_ctx = avcodec_alloc_context3(codec);
  if (impl_->codec_ctx == nullptr) {
    throw std::runtime_error("VideoSender: avcodec_alloc_context3 failed");
  }

  auto * ctx = impl_->codec_ctx;
  const AVRational fps_q = av_d2q(config_.fps, 100000);
  ctx->codec_id = codec->id;
  ctx->codec_type = AVMEDIA_TYPE_VIDEO;
  ctx->width = config_.width;
  ctx->height = config_.height;
  // 灰度模式：编码器只收 Y 平面（GRAY8），省下的 U/V 比特全部给亮度。
  // 实测亮度 SSIM 0.901→0.979（+35% 亮度比特），代价是丢颜色。
  const AVPixelFormat enc_pix_fmt = config_.gray ? AV_PIX_FMT_GRAY8 : AV_PIX_FMT_YUV420P;
  ctx->pix_fmt = enc_pix_fmt;
  ctx->time_base = av_inv_q(fps_q);
  ctx->framerate = fps_q;

  // 码率：单包模式下按「每帧不超过 max_frame_bytes」反推目标码率。
  // 这个值只是给 x264 的目标，真正的硬约束在 encode_and_pack() 里的逐帧超限丢弃。
  //
  // 2026-10-28 画质实验：保守系数 0.85 → 0.90。
  // 0.85 让每帧只敢用 296*0.85≈252 B，剩下 44 B 从来用不到；0.90 让目标
  // 提到 ≈266 B（+6% 信息量）。丢弃阈值仍是 video_max_frame_bytes=296，
  // 所以「超限丢弃」只反映真超限，不会被我们自己压低阈值放大。
  // 观感上约等于 QP 降 1.5 档，是「没那么块」而不是质变。
  // 回退：把 0.90 改回 0.85 即可（只影响这一行，与 yaml 无关）。
  constexpr double kSinglePacketBitrateRatio = 0.90;
  // 大帧拆包模式：原来这里写死成 per_frame_bps = 296×8×fps×0.90（20fps 时 42.6kbps），
  // 也就是「每帧只给 1 个包的预算」。那是 10-28 为了不让 P 帧涨到 2 包才这么保守的。
  // 2026-10-28 晚 CH 要求「把包率用到上限的 85%」，这笔账必须换成令牌桶口径：
  //   预算包率 = send_rate_hz × 0.85（40 → 34 包/秒）
  //   预算码率 = 预算包率 × 296B × 8 ÷ 1.35（见 kLargeFrameLeanFactor）≈ 59.6 kbps
  // 那个 1.35 是「每帧字节向上取整到 296 整数倍」造成的浪费（一帧 400B 也得占 2 个包）。
  // 实测 192x152 GOP10 60kbps：均值 371B/帧、1.777 包/帧 → 每包只装 208.9B 有效数据，
  // 按 296B 口径算就是 1.417 倍的浪费。1.35 故意取比实测小一点，用一点余量换比特；
  // 是否安全靠令牌桶仿真确认（60kbps → 35.5 包/秒 = 88.8% 桶，60 秒 0 丢帧；
  // 74kbps → 41.1 包/秒 = 102.8% 桶，已在净流出）。所以这里不再夹「每帧 1 包」，只夹令牌桶总量。
  std::int64_t bitrate = static_cast<std::int64_t>(config_.bitrate_kbps) * 1000LL;
  std::size_t frame_cap = kMaxPayloadBytes;
  if (config_.large_frame) {
    // 单帧硬上限 = 包数上限 × 296
    frame_cap = static_cast<std::size_t>(std::clamp(config_.max_frame_bytes, 32, 296)) *
                static_cast<std::size_t>(std::clamp(config_.max_frame_packets, 1, 16));
    // 令牌桶口径：85% 的包率预算，再按实测的「码率→包率」系数折成码率。
    //   20fps 时：40 × 0.85 = 34 包/秒；34 × 296 × 8 ÷ 1.35 ≈ 59.6 kbps（日志显示 59）。
    // 实测该码率落 35.5 包/秒（192x152 GOP10 灰度）= 令牌桶的 88.8%，留 11% 余量；
    // 74kbps 会跑到 41.1 包/秒 = 102.8%，一波动就积压丢整帧。
    const double token_bps_d = config_.send_rate_hz * kLargeFrameBudgetRatio *
                               static_cast<double>(kMaxPayloadBytes) * 8.0 /
                               kLargeFrameLeanFactor;
    const std::int64_t token_bps = static_cast<std::int64_t>(token_bps_d);
    if (config_.bitrate_kbps <= 0) {
      bitrate = token_bps;
    } else {
      bitrate = std::min(bitrate, token_bps);
    }
    bitrate = std::max<std::int64_t>(bitrate, 1000);
  } else if (config_.single_packet) {
    frame_cap = static_cast<std::size_t>(std::clamp(config_.max_frame_bytes, 32, 296));
    const std::int64_t cap_bps = static_cast<std::int64_t>(
      static_cast<double>(frame_cap) * 8.0 * config_.fps * kSinglePacketBitrateRatio);
    if (config_.bitrate_kbps <= 0) {
      bitrate = cap_bps;  // 配置写 0（或负数）= 自动
    } else {
      bitrate = std::min(bitrate, cap_bps);
    }
    bitrate = std::max<std::int64_t>(bitrate, 1000);
  }
  ctx->bit_rate = bitrate;
  ctx->rc_min_rate = bitrate;
  ctx->rc_max_rate = bitrate;
  // 单包模式必须用很小的 VBV：默认 buf 太大时，x264 会在前几帧花光码率预算，
  // 之后每帧都往 cap 上顶，超限丢弃的帧会变多。
  //
  // 大帧模式的 VBV 也要收紧（2026-10-28 实测）：
  //   原来给 1 秒 buf（≈65k bit）时，I 帧会冲到 3.3 KB = 12 个包，
  //   超过 8 包上限 → 整帧丢弃 → 而且 12 包的 I 帧要攒 0.3 秒才够令牌，会把整条流水线卡住。
  //   收到「一帧满包数」的字节量（8×296B = 18944 bit）后，实测（300 帧真实赛场画面）：
  //     中位 312B、最大 1565B = 6 包、35.5 包/秒、SSIM 0.9853；超 8 包的帧数 = 0
  //   —— I 帧再也超不过上限，包率也稳在 89% 预算以内。
  const std::int64_t large_frame_vbv = static_cast<std::int64_t>(
    std::clamp(config_.max_frame_packets, 1, 16)) *
    static_cast<std::int64_t>(std::clamp(config_.max_frame_bytes, 32, 296)) * 8;
  const int gop_effective = config_.large_frame ? std::max(1, config_.gop) : 1;
  impl_->gop = gop_effective;
  ctx->rc_buffer_size = config_.single_packet
                          ? std::max<std::int64_t>(bitrate / 2, 1000)
                          : (config_.large_frame ? large_frame_vbv
                                                 : static_cast<std::int64_t>(config_.bitrate_kbps) * 500);
  ctx->gop_size = config_.large_frame ? gop_effective
                                      : (config_.single_packet ? 1 : std::max(1, config_.keyframe_interval));
  ctx->max_b_frames = 0;
  ctx->thread_count = 0;
  ctx->keyint_min = gop_effective;

  av_opt_set(ctx->priv_data, "preset", config_.x264_preset.c_str(), 0);
  av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
  // ★ profile（2026-10-28 改正，两件事）
  // 1) 灰度 = H.264 的 4:0:0（只编亮度平面），baseline / main 都不支持 4:0:0，
  //    只有 High 能编。原来这里写死 baseline —— 一开 video_gray 就会在
  //    avcodec_open2 报 "baseline profile doesn't support 4:0:0"，程序直接起不来。
  // 2) 彩色也不再强制 baseline。实测同码率 65kbps（192x152 GOP10，300 帧真实画面）：
  //      baseline（无 CABAC）SSIM 0.9823 → high（CABAC）SSIM 0.9854
  //    baseline 会关掉 CABAC 这个最值钱的压缩工具，白白亏 3 个点的画质。
  //    所以统一不设 profile，让 x264 自己选（彩色 High / 灰度 High 4:0:0）。
  //    实测码流确认：彩色也是 profile_idc=100，客户端 FFmpeg 软解没问题。
  // 注意别用 priv_data 的 "profile" 字符串选项或 FF_PROFILE_H264_* 宏：
  //   libx264 包装层没有这个私有选项，而 FF_PROFILE_* 在新 FFmpeg 里已改名为 AV_PROFILE_*。
  //   直接留空 = 交给 x264 自动决定，两种 FFmpeg 版本都能编过。
  const char * profile = config_.gray ? "High 4:0:0(自动)" : "High(自动)";
  // repeat-headers=1 + annexb=1：每个关键帧前重复 SPS/PPS，客户端中途加入也能解；
  // aq-mode=1：按宏块复杂度分配码率，纹理区给更多比特（提升主观清晰度）
  // 单包模式额外 keyint=1/min-keyint=1：每一帧都是 IDR，任意一个包单独到了都能解出图。
  std::string x264_params =
    "repeat-headers=1:annexb=1:bframes=0:scenecut=0:force-cfr=1:aq-mode=1:aq-strength=1.2";
  if (config_.single_packet) x264_params += ":keyint=1:min-keyint=1";
  if (config_.large_frame) {
    x264_params += ":keyint=" + std::to_string(gop_effective) +
                   ":min-keyint=" + std::to_string(gop_effective);
    // 大帧模式要抗丢包：开启 slice 分片，让丢失一个包只毁掉一部分画面而不是整帧。
    // 实测 160×128 GOP5：不加分片 P 帧 155B / 加分片 149B，画质不变（0.9945 → 0.9950），
    // 但每条横带独立熵编码，丢包只坏一条。注意别加 intra-refresh：实测 SSIM 掉到 0.9867，
    // P 帧还涨到 278B（它把刷新分摊到每帧，等于把 I 帧摊薄）。
    x264_params += ":slices=4";
  }
  av_opt_set(ctx->priv_data, "x264-params", x264_params.c_str(), 0);

  const int ret = avcodec_open2(ctx, codec, nullptr);
  if (ret < 0) {
    throw std::runtime_error("VideoSender: avcodec_open2 failed: " + ffmpeg_err(ret));
  }

  impl_->frame = av_frame_alloc();
  impl_->packet = av_packet_alloc();
  if (!impl_->frame || !impl_->packet) {
    throw std::runtime_error("VideoSender: alloc frame/packet failed");
  }
  impl_->frame_cap = frame_cap;
  impl_->frame->format = ctx->pix_fmt;
  impl_->frame->width = ctx->width;
  impl_->frame->height = ctx->height;
  if (av_frame_get_buffer(impl_->frame, 32) < 0) {
    throw std::runtime_error("VideoSender: av_frame_get_buffer failed");
  }

  // sws 只做「颜色空间转换」，不做缩放：取景/缩放已经在 encode_and_pack 里用
  // cv::resize 做完了。src 和 dst 尺寸相同，这里只负责 BGR→YUV/GRAY。
  impl_->sws_ctx = sws_getContext(
    config_.width, config_.height, AV_PIX_FMT_BGR24,
    config_.width, config_.height, enc_pix_fmt,
    SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (impl_->sws_ctx == nullptr) {
    throw std::runtime_error("VideoSender: sws_getContext failed");
  }

  tools::logger()->info(
    "[VideoSender] 初始化完成 {}x{} @{:.0f}fps {}kbps preset={} sharpen={} 灰度={} profile={}",
    config_.width, config_.height, config_.fps, bitrate / 1000,
    config_.x264_preset, config_.sharpen, config_.gray ? "是" : "否", profile);
  if (config_.large_frame) {
    tools::logger()->info(
      "[VideoSender] 大帧拆包模式（I+P）：GOP={} 单帧上限 {} 字节（最多 {} 包），"
      "令牌桶 {:.0f}包/秒（码率按其中 {:.0f}% 给 = 约 {:.1f} 包/秒），VBV={} bit。"
      "丢一个包只毁一条 slice；P 帧依赖前面的 I 帧。",
      gop_effective, frame_cap, std::clamp(config_.max_frame_packets, 1, 16),
      config_.send_rate_hz, kLargeFrameBudgetRatio * 100.0,
      config_.send_rate_hz * kLargeFrameBudgetRatio, large_frame_vbv);
  } else if (config_.single_packet) {
    tools::logger()->info(
      "[VideoSender] 单包模式：每帧独立编码（全 I 帧），整帧必须 ≤ {} 字节，"
      "超限整帧丢弃（绝不拆包）。编码失败或超限时会打日志。",
      frame_cap);
  }

  // 编码放到独立线程：preset=slow 在 Jetson 上一帧约 40~70ms，
  // 放在自瞄主循环里会严重拖慢云台控制（实测 10fps 时主循环被拉到 149ms/轮）。
  impl_->worker = std::thread(&VideoSender::worker_loop, this);
}

VideoSender::~VideoSender()
{
  if (!impl_) return;
  {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->quit = true;
  }
  impl_->cv.notify_all();
  if (impl_->worker.joinable()) impl_->worker.join();
}

std::vector<std::vector<std::uint8_t>> VideoSender::feed(
  const cv::Mat & bgr_frame,
  std::int64_t timestamp_ns)
{
  (void)timestamp_ns;
  std::vector<std::vector<std::uint8_t>> out;
  if (!bgr_frame.empty()) {
    // 节流：按 fps 只接收新帧；同一时刻只保留最新一帧，编码跟不上就丢旧帧，
    // 主线程不会阻塞在编码上（编码由工作线程完成）。
    //
    // ★ 2026-10-28 修正：原来用 "now - last_send >= interval" 再 last_send = now，
    //   把发送间隔量化到主循环周期的整数倍。主循环 T≈40ms、目标 1/20fps=50ms，
    //   50ms 落在 1T~2T 之间 → 每次都等到 2T≈80ms → 实际只有 10.8~12.5fps。
    //   改成「按固定节拍累加」：next_send 每次只前进一个 interval，追不上才夹回 now。
    const auto now = std::chrono::steady_clock::now();
    const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(1.0 / config_.fps));
    if (!impl_->has_last_send) {
      impl_->last_send = now;
      impl_->has_last_send = true;
    }
    if (now >= impl_->last_send) {
      impl_->last_send += interval;
      // 落后超过一个 interval（主循环卡顿/相机掉帧）就重新对齐，避免之后一直「补发」
      if (impl_->last_send + interval < now) impl_->last_send = now + interval;
      {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        // 不需要 clone：大恒/迈德威视/USB 相机后端都是每帧新建 cv::Mat，
        // 且调用方在本轮之后不再写这块内存，浅拷贝（引用计数）即可，省一次 3.9MB memcpy。
        impl_->pending_frame = bgr_frame;
      }
      impl_->cv.notify_one();
    }
  }
  drain_due(out, false);
  return out;
}

void VideoSender::worker_loop()
{
  while (true) {
    cv::Mat frame;
    {
      std::unique_lock<std::mutex> lock(impl_->mtx);
      impl_->cv.wait(lock, [this] { return impl_->quit || impl_->pending_frame.has_value(); });
      if (impl_->quit) return;
      frame = std::move(*impl_->pending_frame);
      impl_->pending_frame.reset();
      impl_->encoding = true;
    }
    encode_and_pack(frame);
    {
      std::lock_guard<std::mutex> lock(impl_->mtx);
      impl_->encoding = false;
    }
    impl_->cv.notify_all();
  }
}

void VideoSender::encode_and_pack(const cv::Mat & bgr_frame)
{
  if (bgr_frame.empty()) return;

  // ---- 取景：默认整幅缩放（全视野），可选中心裁剪 ----
  cv::Mat view;
  if (config_.keep_aspect) {
    // 等比缩放到 width×height 内，左右或上下用黑边补齐（letterbox）
    view = cv::Mat(config_.height, config_.width, CV_8UC3, cv::Scalar(0, 0, 0));
    const double scale = std::min(
      static_cast<double>(config_.width) / bgr_frame.cols,
      static_cast<double>(config_.height) / bgr_frame.rows);
    const int nw = std::max(1, static_cast<int>(std::lround(bgr_frame.cols * scale)));
    const int nh = std::max(1, static_cast<int>(std::lround(bgr_frame.rows * scale)));
    cv::Mat resized;
    cv::resize(bgr_frame, resized, cv::Size(nw, nh), 0, 0, cv::INTER_AREA);
    resized.copyTo(view(cv::Rect((config_.width - nw) / 2, (config_.height - nh) / 2, nw, nh)));
  } else {
    const int cs = config_.crop_size;
    cv::Mat cropped;
    if (bgr_frame.cols < cs || bgr_frame.rows < cs) {
      cv::resize(bgr_frame, cropped, cv::Size(cs, cs));
    } else {
      cropped = bgr_frame(cv::Rect((bgr_frame.cols - cs) / 2, (bgr_frame.rows - cs) / 2, cs, cs));
    }
    if (cropped.cols != config_.width || cropped.rows != config_.height) {
      cv::resize(cropped, view, cv::Size(config_.width, config_.height), 0, 0, cv::INTER_AREA);
    } else {
      view = cropped.clone();
    }
  }

  // ---- 锐化（USM）：提升主观清晰度，几乎不增加码率 ----
  if (config_.sharpen > 0.0) {
    cv::Mat blurred;
    cv::GaussianBlur(view, blurred, cv::Size(0, 0), 1.2);
    cv::addWeighted(view, 1.0 + config_.sharpen, blurred, -config_.sharpen, 0, view);
  }

  // ---- 转 YUV420P（或灰度）----
  if (av_frame_make_writable(impl_->frame) < 0) return;
  const uint8_t * src_data[1] = {view.data};
  const int src_linesize[1] = {static_cast<int>(view.step)};
  sws_scale(impl_->sws_ctx, src_data, src_linesize, 0, config_.height,
            impl_->frame->data, impl_->frame->linesize);
  impl_->frame->pts = impl_->next_pts++;

  // 自救：强制下一帧出 I 帧（x264 认 pict_type=I + 乘 keyint 偏移来强制 IDR）
  if (config_.large_frame && impl_->force_keyframe) {
    impl_->frame->pict_type = AV_PICTURE_TYPE_I;
    impl_->force_keyframe = false;
  } else {
    impl_->frame->pict_type = AV_PICTURE_TYPE_NONE;
  }

  // ---- 编码 ----
  int ret = avcodec_send_frame(impl_->codec_ctx, impl_->frame);
  if (ret < 0) {
    tools::logger()->warn("[VideoSender] send_frame failed: {}", ffmpeg_err(ret));
    ++impl_->stat_encode_fail;
    return;
  }
  bool got_key = false;
  while (true) {
    ret = avcodec_receive_packet(impl_->codec_ctx, impl_->packet);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
    if (ret < 0) {
      tools::logger()->warn("[VideoSender] receive_packet failed: {}", ffmpeg_err(ret));
      ++impl_->stat_encode_fail;
      break;
    }
    if (impl_->packet->flags & AV_PKT_FLAG_KEY) got_key = true;
    impl_->stream_buf.insert(impl_->stream_buf.end(),
                             impl_->packet->data,
                             impl_->packet->data + impl_->packet->size);
    av_packet_unref(impl_->packet);
  }
  impl_->last_encoded_was_key = got_key;

  // ---- 大帧拆包模式（I+P）：一帧 → N 个 0x0310 包，每个 ≤ 296B ----
  // 客户端不用改：它的 av_parser_parse2 是持久解析器，会跨包累积成一个完整帧。
  // 每个包自带精确长度，最后一包短一点，不会往码流里塞填充 0。
  if (config_.large_frame) {
    const std::size_t sz = impl_->stream_buf.size();
    impl_->stat_last_bytes = sz;
    const std::size_t per = static_cast<std::size_t>(
      std::clamp(config_.max_frame_bytes, 32, 296));
    const std::size_t max_pkts = static_cast<std::size_t>(
      std::clamp(config_.max_frame_packets, 1, 16));
    const bool is_key = impl_->last_encoded_was_key;

    if (sz > 0 && sz <= per * max_pkts) {
      std::vector<std::vector<std::uint8_t>> packets;
      std::size_t off = 0;
      while (off < sz) {
        const std::size_t n = std::min(per, sz - off);
        packets.push_back(make_transport_packet(impl_->stream_buf.data() + off, n));
        off += n;
      }
      ++impl_->stat_frames;
      impl_->stat_last_packets = packets.size();
      if (is_key) {
        ++impl_->stat_key_frames;
      } else {
        ++impl_->stat_p_frames;
      }
      {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        for (auto & p : packets) impl_->pending.push_back(std::move(p));
        impl_->pending_frames.push_back(packets.size());
      }
      // 丢包恢复：P 帧依赖前面的 I 帧。如果上一个 I 帧被丢/超限，
      // 后面的 P 帧全都在引用不存在的参考帧，客户端会一直花屏。
      // 超过一个 GOP 还没成功发出一整个 I 帧，就强制下一帧重新出 I 帧。
      if (is_key) {
        impl_->frames_since_key = 0;
      } else if (++impl_->frames_since_key > impl_->gop * 2) {
        impl_->force_keyframe = true;
        impl_->frames_since_key = 0;
        tools::logger()->warn(
          "[VideoSender] 大帧模式：连续 {} 帧没能发出 I 帧，强制下一帧重出 I 帧",
          impl_->gop * 2);
      }
    } else if (sz > per * max_pkts) {
      ++impl_->stat_oversize;
      if (is_key) {
        ++impl_->stat_key_dropped;
        // I 帧本身塞不进「包数上限×296」= 配置问题（码率/分辨率给太大）。
        // 此时强制再出 I 帧也还是塞不进，会变成死循环，所以只报警，等自然 GOP
        // 的下一个 I 帧；中间最多花屏 gop-1 帧（GOP5 = 200ms）。
      } else {
        // ★ P 帧被丢：它后面的 P 帧全都引用这个不存在的参考帧，而客户端的
        //   丢包检测看的是内层序号——发送端压根没给这一帧分配序号，序号是连续的，
        //   客户端**不会**察觉到丢帧，会一直解出花屏直到下一个 I 帧。
        //   所以这里必须显式让下一帧出 I 帧，把损伤压到 1 帧以内。
        impl_->force_keyframe = true;
        impl_->frames_since_key = 0;
        tools::logger()->warn(
          "[VideoSender] 大帧模式：P 帧超限被丢（{} 字节 > {}），下一帧强制重出 I 帧",
          sz, per * max_pkts);
      }
    }
    impl_->stream_buf.clear();
    impl_->stream_offset = 0;

    const auto now = std::chrono::steady_clock::now();
    if (!impl_->has_stat_last_log || now - impl_->stat_last_log >= std::chrono::seconds(5)) {
      impl_->stat_last_log = now;
      impl_->has_stat_last_log = true;
      tools::logger()->info(
        "[VideoSender] 大帧统计：成帧 {}（I {} / P {}）超限丢弃 {}（I {}） 出包丢包 {} 编码错误 {}  "
        "最近帧 {} 字节 / {} 包（单包上限 {}，单帧上限 {}）",
        impl_->stat_frames, impl_->stat_key_frames, impl_->stat_p_frames,
        impl_->stat_oversize, impl_->stat_key_dropped, impl_->stat_pkt_lost,
        impl_->stat_encode_fail,
        impl_->stat_last_bytes, impl_->stat_last_packets, per, per * max_pkts);
      // ★ 出包丢包变了就第一时间报警。这一项涨说明「令牌桶撑不住码率」——
      //   会积压 → 丢整帧 → 断参考链，画面一顿一顿的。这时该把码率往下调，
      //   而不是往上调。2026-10-28 实测：预算按 85% 包率给时这一项应该是 0。
      if (impl_->stat_pkt_lost > impl_->stat_pkt_lost_logged) {
        impl_->stat_pkt_lost_logged = impl_->stat_pkt_lost;
        tools::logger()->warn(
          "[VideoSender] 出包跟不上：累计丢了 {} 个包（含整帧）。说明码率超过了 "
          "{:.0f}包/秒 令牌桶的实际能力，把 video_bitrate_kbps 往下调 5~10。",
          impl_->stat_pkt_lost, config_.send_rate_hz);
      }
      if (impl_->stat_oversize > impl_->stat_oversize_logged) {
        impl_->stat_oversize_logged = impl_->stat_oversize;
        tools::logger()->warn(
          "[VideoSender] 有帧超限：说明视频码率给大了（per 包 × {} 包 = {} 字节装不下）。"
          "I 帧会按自然 GOP 重来（最多花屏 {} 帧），P 帧超限则会立刻强制下一帧出 I 帧。"
          "持续出现就改视频码率或分辨率。",
          max_pkts, per * max_pkts, impl_->gop - 1);
      }
    }
    return;
  }

  // ---- 单包模式：一帧 == 一个 0x0310 包；塞不下就整帧丢掉，绝不拆包 ----
  // 链路通过率只有千分之几，多包的大帧永远拼不完整；而全 I 帧编码让每个包
  // 自带 SPS/PPS + IDR，任何一个包单独到达都能解出一张完整的图。
  if (config_.single_packet) {
    const std::size_t sz = impl_->stream_buf.size();
    impl_->stat_last_bytes = sz;
    if (sz > 0 && sz <= impl_->frame_cap) {
      auto tp = make_transport_packet(impl_->stream_buf.data(), sz);
      ++impl_->stat_frames;
      std::lock_guard<std::mutex> lock(impl_->mtx);
      impl_->pending.push_back(std::move(tp));
    } else if (sz > impl_->frame_cap) {
      ++impl_->stat_oversize;
    }
    impl_->stream_buf.clear();
    impl_->stream_offset = 0;

    // 每 5 秒打一次统计：没有这行日志就没法判断"一帧一包"有没有生效
    const auto now = std::chrono::steady_clock::now();
    if (!impl_->has_stat_last_log || now - impl_->stat_last_log >= std::chrono::seconds(5)) {
      impl_->stat_last_log = now;
      impl_->has_stat_last_log = true;
      tools::logger()->info(
        "[VideoSender] 1包/帧 统计：成帧 {} 超限丢弃 {} 编码错误 {}  最近帧 {} 字节（上限 {}）",
        impl_->stat_frames, impl_->stat_oversize, impl_->stat_encode_fail,
        impl_->stat_last_bytes, impl_->frame_cap);
    }
    return;
  }

  // ---- 多包模式（旧逻辑）：每 296 字节一个内层包 → 一个 300 字节 transport packet ----
  // 注意：剩余不足 296 字节的尾巴必须在本帧立即封包（补 0 到 300），
  //     不能跨帧拼接：客户端每个 0x0310 包独立解析，内层 length 为 0 即视为
  //     填充截止；如果下帧数据接到本包填充后面，会永久丢失。
  std::deque<std::vector<std::uint8_t>> local;
  while (impl_->stream_offset < impl_->stream_buf.size()) {
    const std::size_t remain = impl_->stream_buf.size() - impl_->stream_offset;
    const std::size_t n = std::min(remain, kMaxPayloadBytes);
    local.push_back(make_transport_packet(impl_->stream_buf.data() + impl_->stream_offset, n));
    impl_->stream_offset += n;
  }
  impl_->stream_buf.clear();
  impl_->stream_offset = 0;

  if (!local.empty()) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    for (auto & p : local) impl_->pending.push_back(std::move(p));
  }
}

void VideoSender::drain_due(std::vector<std::vector<std::uint8_t>> & out, bool force)
{
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(impl_->mtx);

  // 令牌桶：按真实流逝时间积令牌，保证长期平均严格 = send_rate_hz 包/秒。
  // 上限：单包模式攒 4 个就够（突发 ≈ 100ms）；大帧模式一个 I 帧就要 3~4 个，
  // 上限必须放到「单帧最大包数」，否则永远攒不够、一帧也放不出去。
  const double token_cap = config_.large_frame
                             ? static_cast<double>(std::clamp(config_.max_frame_packets, 1, 16))
                             : 4.0;
  if (!impl_->has_last_tick) {
    impl_->last_tick = now;
    impl_->has_last_tick = true;
  }
  const double elapsed_s = std::chrono::duration<double>(now - impl_->last_tick).count();
  impl_->last_tick = now;
  impl_->tokens = std::min(token_cap, impl_->tokens + elapsed_s * config_.send_rate_hz);

  // ★ 大帧模式：一帧拆成 N 个包，这 N 个包必须「一起」连续放出去。
  //   如果还把包一个一个往节拍上挤，一个 I 帧要跨 3~4 个节拍（约 100ms），
  //   而且客户端的解析器会把下一帧的数据提前接到这个 I 帧后面——拼出坏帧。
  //   所以按「整帧原子释放」：令牌够这一帧的全部包，就一口气全放；不够就全攒着。
  if (config_.large_frame) {
    if (force) {
      // flush：不管令牌，把所有帧原样放完
      out.reserve(out.size() + impl_->pending.size());
      while (!impl_->pending.empty()) {
        out.push_back(std::move(impl_->pending.front()));
        impl_->pending.pop_front();
      }
      impl_->pending_frames.clear();
      return;
    }
    while (!impl_->pending_frames.empty()) {
      const std::size_t n = impl_->pending_frames.front();
      if (impl_->tokens < static_cast<double>(n)) break;
      if (impl_->pending.size() < n) break;  // 防御：账目对不上时不动
      for (std::size_t i = 0; i < n; ++i) {
        out.push_back(std::move(impl_->pending.front()));
        impl_->pending.pop_front();
      }
      impl_->tokens -= static_cast<double>(n);
      impl_->pending_frames.pop_front();
    }
    // 兜底：积压超过 1 秒的帧量就丢最老的整帧（保持延迟不膨胀）
    while (impl_->pending_frames.size() > static_cast<std::size_t>(std::max(2.0, config_.fps)))
    {
      const std::size_t n = impl_->pending_frames.front();
      for (std::size_t i = 0; i < n && !impl_->pending.empty(); ++i) {
        impl_->pending.pop_front();
        ++impl_->stat_pkt_lost;
      }
      impl_->pending_frames.pop_front();
      if (now - impl_->last_drop_log >= std::chrono::seconds(5)) {
        impl_->last_drop_log = now;
        tools::logger()->warn(
          "[VideoSender] 出包跟不上，已丢弃 {} 个最老的整帧（{:.0f} 包/秒 令牌不够）",
          impl_->pending_frames.size(), config_.send_rate_hz);
      }
    }
    return;
  }

  // force=true（flush）时忽略令牌，全部放出
  while (!impl_->pending.empty() && (force || impl_->tokens >= 1.0)) {
    out.push_back(std::move(impl_->pending.front()));
    impl_->pending.pop_front();
    if (!force) impl_->tokens -= 1.0;
  }

  // 兜底：待发包超过 20 个（0.5 秒的量）说明出包跟不上，丢弃最老的，
  // 保证延迟不会无限累加（丢的是最老的包，客户端最多短暂花屏一下）。
  // 正常情况下 90kbps 编码 ≈ 40 包/秒，刚好跟 40Hz 令牌桶打平。
  std::uint64_t dropped = 0;
  while (impl_->pending.size() > 20U) {
    impl_->pending.pop_front();
    ++dropped;
  }
  if (dropped > 0) {
    const auto now2 = std::chrono::steady_clock::now();
    if (now2 - impl_->last_drop_log >= std::chrono::seconds(5)) {
      impl_->last_drop_log = now2;
      tools::logger()->warn("[VideoSender] 出包跟不上 40Hz 节拍，已丢弃 {} 个最老的包（码率过高或主循环太慢）",
                            dropped);
    }
  }
}

std::vector<std::vector<std::uint8_t>> VideoSender::flush()
{
  std::vector<std::vector<std::uint8_t>> out;
  if (!impl_) return out;

  // 先让工作线程把已入队的帧编码完（最多等 200ms），再无条件放出全部包
  {
    std::unique_lock<std::mutex> lock(impl_->mtx);
    impl_->cv.wait_for(lock, std::chrono::milliseconds(300),
                       [this] { return !impl_->pending_frame.has_value() && !impl_->encoding; });
  }

  drain_due(out, true);
  return out;
}

// make_transport_packet 封包（结构必须与客户端解包逻辑一致）
std::vector<std::uint8_t> VideoSender::make_transport_packet(
  const std::uint8_t * payload, std::size_t payload_size)
{
  std::vector<std::uint8_t> tp(kTransportPacketBytes, 0);

  const std::size_t n = std::min(payload_size, kMaxPayloadBytes);
  const std::size_t inner_len = kInnerSeqBytes + n;

  write_u16_be(tp.data(), static_cast<std::uint16_t>(inner_len));
  write_u16_be(tp.data() + kTransportLengthPrefix, impl_->inner_seq++);
  if (payload != nullptr && n > 0) {
    std::memcpy(tp.data() + kTransportLengthPrefix + kInnerSeqBytes, payload, n);
  }

  // 封进 0x0310 帧
  std::vector<std::uint8_t> f(kVideoFrameBytes, 0);
  f[0] = kFrameSOF;
  write_u16_le(f.data() + 1, static_cast<std::uint16_t>(kTransportPacketBytes));
  f[3] = impl_->outer_seq++;
  f[4] = tools::get_crc8(f.data(), 4);
  write_u16_le(f.data() + 5, kCmdId);
  std::memcpy(f.data() + kVideoFrameHeaderBytes, tp.data(), kTransportPacketBytes);
  const std::uint16_t crc16 =
    tools::get_crc16(f.data(), kVideoFrameHeaderBytes + kTransportPacketBytes);
  write_u16_le(f.data() + kVideoFrameHeaderBytes + kTransportPacketBytes, crc16);
  return f;
}

}  // namespace io
