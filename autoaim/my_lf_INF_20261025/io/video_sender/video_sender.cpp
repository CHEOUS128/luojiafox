#include "video_sender.hpp"

#include "tools/crc.hpp"
#include "tools/logger.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

namespace
{

// 0x0310 帧的固定字段
constexpr std::uint8_t kFrameSOF = 0xA5;
constexpr std::uint16_t kCmdId = 0x0310;
constexpr std::size_t kDataLen = 300;   // 用 size_t 避免与 size_t 运算时类型推导失败

void write_u16_le(std::uint8_t * out, std::uint16_t v)
{
  out[0] = static_cast<std::uint8_t>(v & 0xFF);
  out[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
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

  std::vector<std::uint8_t> stream_buf;   // 编码输出的累积字节
  std::size_t stream_offset = 0;          // 已切包消费到的位置
  std::uint8_t seq = 0;                   // 0x0310 包序号
  std::int64_t next_pts = 0;

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

  // ---- 查找 H.264 编码器 ----
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
  ctx->rc_buffer_size = static_cast<std::int64_t>(config_.bitrate_kbps) * 300;  // 0.3s
  ctx->gop_size = std::max(1, config_.keyframe_interval);
  ctx->max_b_frames = 0;      // zerolatency 必须无 B 帧
  ctx->thread_count = 0;
  ctx->keyint_min = 1;

  av_opt_set(ctx->priv_data, "preset", config_.x264_preset.c_str(), 0);
  av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
  av_opt_set(ctx->priv_data, "profile", "baseline", 0);
  av_opt_set(ctx->priv_data, "x264-params", "bframes=0:scenecut=0", 0);

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
    config_.crop_size, config_.crop_size, AV_PIX_FMT_BGR24,
    config_.width, config_.height, AV_PIX_FMT_YUV420P,
    SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
  if (impl_->sws_ctx == nullptr) {
    throw std::runtime_error("VideoSender: sws_getContext failed");
  }

  tools::logger()->info(
    "[VideoSender] 初始化完成 {}x{} @{:.0f}fps {}kbps preset={}",
    config_.width, config_.height, config_.fps, config_.bitrate_kbps, config_.x264_preset);
}

VideoSender::~VideoSender() = default;

std::vector<std::vector<std::uint8_t>> VideoSender::feed(
  const cv::Mat & bgr_frame,
  std::int64_t timestamp_ns)
{
  (void)timestamp_ns;
  std::vector<std::vector<std::uint8_t>> out;
  if (bgr_frame.empty()) return out;

  // ---- 节流：按 fps 限制 ----
  const auto now = std::chrono::steady_clock::now();
  const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
    std::chrono::duration<double>(1.0 / config_.fps));
  if (impl_->has_last_send && (now - impl_->last_send) < interval) {
    return out;
  }
  impl_->last_send = now;
  impl_->has_last_send = true;

  // ---- 中心裁剪到 crop_size × crop_size ----
  cv::Mat cropped;
  const int cs = config_.crop_size;
  if (bgr_frame.cols < cs || bgr_frame.rows < cs) {
    // 原图比裁剪尺寸小，直接缩放（异常情况）
    cv::resize(bgr_frame, cropped, cv::Size(cs, cs));
  } else {
    const int x = (bgr_frame.cols - cs) / 2;
    const int y = (bgr_frame.rows - cs) / 2;
    cropped = bgr_frame(cv::Rect(x, y, cs, cs));
  }

  // ---- 转 YUV420P ----
  if (av_frame_make_writable(impl_->frame) < 0) {
    return out;
  }
  const uint8_t * src_data[1] = {cropped.data};
  const int src_linesize[1] = {static_cast<int>(cropped.step)};
  sws_scale(impl_->sws_ctx, src_data, src_linesize, 0, cs,
            impl_->frame->data, impl_->frame->linesize);
  impl_->frame->pts = impl_->next_pts++;

  // ---- 编码 ----
  int ret = avcodec_send_frame(impl_->codec_ctx, impl_->frame);
  if (ret < 0) {
    tools::logger()->warn("[VideoSender] send_frame failed: {}", ffmpeg_err(ret));
    return out;
  }
  while (true) {
    ret = avcodec_receive_packet(impl_->codec_ctx, impl_->packet);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
    if (ret < 0) {
      tools::logger()->warn("[VideoSender] receive_packet failed: {}", ffmpeg_err(ret));
      break;
    }
    impl_->stream_buf.insert(
      impl_->stream_buf.end(),
      impl_->packet->data,
      impl_->packet->data + impl_->packet->size);
    av_packet_unref(impl_->packet);
  }

  // ---- 切包：每包 300 字节 ----
  while (impl_->stream_buf.size() - impl_->stream_offset >= kDataLen) {
    out.push_back(make_frame(
      impl_->stream_buf.data() + impl_->stream_offset, kDataLen));
    impl_->stream_offset += kDataLen;
  }
  // 压缩剩余字节
  if (impl_->stream_offset > 0) {
    impl_->stream_buf.erase(
      impl_->stream_buf.begin(),
      impl_->stream_buf.begin() + static_cast<std::ptrdiff_t>(impl_->stream_offset));
    impl_->stream_offset = 0;
  }
  return out;
}

std::vector<std::vector<std::uint8_t>> VideoSender::flush()
{
  std::vector<std::vector<std::uint8_t>> out;
  if (!impl_ || !impl_->codec_ctx) return out;

  avcodec_send_frame(impl_->codec_ctx, nullptr);  // 刷出剩余帧
  while (true) {
    const int ret = avcodec_receive_packet(impl_->codec_ctx, impl_->packet);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
    if (ret < 0) break;
    impl_->stream_buf.insert(
      impl_->stream_buf.end(),
      impl_->packet->data,
      impl_->packet->data + impl_->packet->size);
    av_packet_unref(impl_->packet);
  }
  // 最后不足 300 字节的尾巴补 0 发出
  while (impl_->stream_offset < impl_->stream_buf.size()) {
    const std::size_t remain = impl_->stream_buf.size() - impl_->stream_offset;
    if (remain == 0) break;
    std::vector<std::uint8_t> payload(kDataLen, 0);
    const std::size_t n = std::min(remain, kDataLen);
    std::memcpy(payload.data(), impl_->stream_buf.data() + impl_->stream_offset, n);
    impl_->stream_offset += n;
    out.push_back(make_frame(payload.data(), kDataLen));
  }
  impl_->stream_buf.clear();
  impl_->stream_offset = 0;
  return out;
}

// make_frame 把 300 字节负载包成 309 字节的 0x0310 帧
std::vector<std::uint8_t> VideoSender::make_frame(
  const std::uint8_t * payload, std::size_t payload_size)
{
  std::vector<std::uint8_t> f(kVideoFrameBytes, 0);
  f[0] = kFrameSOF;
  write_u16_le(f.data() + 1, static_cast<std::uint16_t>(kDataLen));
  f[3] = impl_->seq++;                     // 包序号自增
  f[4] = tools::get_crc8(f.data(), 4);     // CRC8 覆盖前 4 字节
  write_u16_le(f.data() + 5, kCmdId);      // 0x0310
  // 拷贝负载（不足 300 补 0，调用方保证 payload_size <= 300）
  const std::size_t n = std::min(payload_size, kDataLen);
  if (payload != nullptr && n > 0) {
    std::memcpy(f.data() + kVideoFrameHeaderBytes, payload, n);
  }
  // CRC16 覆盖前 307 字节
  const std::uint16_t crc16 = tools::get_crc16(
    f.data(), kVideoFrameHeaderBytes + kDataLen);
  write_u16_le(f.data() + kVideoFrameHeaderBytes + kDataLen, crc16);
  return f;
}

}  // namespace io
