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

// 发送节拍：与 ACE 编码端 center_crop_packet_send_rate_hz=40 一致。
// 40Hz × 296B ≈ 94.7kbps 是这条通道的实际上限；按节拍发可以避免
// 一帧内猛发把下位机/VT03 的接收缓冲打爆。
constexpr double kSendRateHz = 40.0;

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

  // ---- 主线程与工作线程共享 ----
  std::mutex mtx;
  std::condition_variable cv;
  std::optional<cv::Mat> pending_frame;   // 待编码帧（只保留最新一帧，慢于 fps 时自动丢旧帧）
  bool encoding = false;                  // 工作线程是否正在编码（flush 需要等它结束）
  std::deque<std::vector<std::uint8_t>> pending;  // 已封好、等待 40Hz 节拍放出的 0x0310 帧
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
  ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  ctx->time_base = av_inv_q(fps_q);
  ctx->framerate = fps_q;
  ctx->bit_rate = static_cast<std::int64_t>(config_.bitrate_kbps) * 1000LL;
  ctx->rc_min_rate = ctx->bit_rate;
  ctx->rc_max_rate = ctx->bit_rate;
  ctx->rc_buffer_size = static_cast<std::int64_t>(config_.bitrate_kbps) * 500;  // 0.5s
  ctx->gop_size = std::max(1, config_.keyframe_interval);
  ctx->max_b_frames = 0;
  ctx->thread_count = 0;
  ctx->keyint_min = 1;

  av_opt_set(ctx->priv_data, "preset", config_.x264_preset.c_str(), 0);
  av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
  av_opt_set(ctx->priv_data, "profile", "baseline", 0);
  // repeat-headers=1 + annexb=1：每个关键帧前重复 SPS/PPS，客户端中途加入也能解；
  // aq-mode=1：按宏块复杂度分配码率，纹理区给更多比特（提升主观清晰度）
  av_opt_set(ctx->priv_data, "x264-params",
             "repeat-headers=1:annexb=1:bframes=0:scenecut=0:force-cfr=1:"
             "aq-mode=1:aq-strength=1.2", 0);

  const int ret = avcodec_open2(ctx, codec, nullptr);
  if (ret < 0) {
    throw std::runtime_error("VideoSender: avcodec_open2 failed: " + ffmpeg_err(ret));
  }

  impl_->frame = av_frame_alloc();
  impl_->packet = av_packet_alloc();
  if (!impl_->frame || !impl_->packet) {
    throw std::runtime_error("VideoSender: alloc frame/packet failed");
  }
  impl_->frame->format = ctx->pix_fmt;
  impl_->frame->width = ctx->width;
  impl_->frame->height = ctx->height;
  if (av_frame_get_buffer(impl_->frame, 32) < 0) {
    throw std::runtime_error("VideoSender: av_frame_get_buffer failed");
  }

  impl_->sws_ctx = sws_getContext(
    config_.width, config_.height, AV_PIX_FMT_BGR24,
    config_.width, config_.height, AV_PIX_FMT_YUV420P,
    SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (impl_->sws_ctx == nullptr) {
    throw std::runtime_error("VideoSender: sws_getContext failed");
  }

  tools::logger()->info(
    "[VideoSender] 初始化完成 {}x{} @{:.0f}fps {}kbps preset={} sharpen={}",
    config_.width, config_.height, config_.fps, config_.bitrate_kbps,
    config_.x264_preset, config_.sharpen);

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
    const auto now = std::chrono::steady_clock::now();
    const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(1.0 / config_.fps));
    if (!impl_->has_last_send || (now - impl_->last_send) >= interval) {
      impl_->last_send = now;
      impl_->has_last_send = true;
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

  // ---- 转 YUV420P ----
  if (av_frame_make_writable(impl_->frame) < 0) return;
  const uint8_t * src_data[1] = {view.data};
  const int src_linesize[1] = {static_cast<int>(view.step)};
  sws_scale(impl_->sws_ctx, src_data, src_linesize, 0, config_.height,
            impl_->frame->data, impl_->frame->linesize);
  impl_->frame->pts = impl_->next_pts++;

  // ---- 编码 ----
  int ret = avcodec_send_frame(impl_->codec_ctx, impl_->frame);
  if (ret < 0) {
    tools::logger()->warn("[VideoSender] send_frame failed: {}", ffmpeg_err(ret));
    return;
  }
  while (true) {
    ret = avcodec_receive_packet(impl_->codec_ctx, impl_->packet);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
    if (ret < 0) {
      tools::logger()->warn("[VideoSender] receive_packet failed: {}", ffmpeg_err(ret));
      break;
    }
    impl_->stream_buf.insert(impl_->stream_buf.end(),
                             impl_->packet->data,
                             impl_->packet->data + impl_->packet->size);
    av_packet_unref(impl_->packet);
  }

  // ---- 打包：每 296 字节一个内层包 → 一个 300 字节 transport packet ----
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

  // 令牌桶：按真实流逝时间积令牌（上限 4 个），保证长期平均严格 = 40 包/秒
  constexpr double kTokenBurst = 4.0;
  if (!impl_->has_last_tick) {
    impl_->last_tick = now;
    impl_->has_last_tick = true;
  }
  const double elapsed_s = std::chrono::duration<double>(now - impl_->last_tick).count();
  impl_->last_tick = now;
  impl_->tokens = std::min(kTokenBurst, impl_->tokens + elapsed_s * kSendRateHz);

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
