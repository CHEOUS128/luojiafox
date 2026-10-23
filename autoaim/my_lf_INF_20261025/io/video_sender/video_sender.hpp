// video_sender.hpp —— 自瞄工业相机画面 → 0x0310 视频帧
//
// 作用：把自瞄主循环读到的 BGR 图像，缩放到 300×240（默认全视野、无黑边）、
//       在独立线程里 H.264 编码、切成 300 字节定长包，再包成官方 0x0310 图传帧
//       （309 字节），按 40Hz 节拍通过 USB CDC 串口发给下位机。
//
// 协议依据：《RoboMaster 2026 高校系列赛通信协议 V2.0.0》表 1-41
//
// 0x0310 帧格式（309 字节）：
//   [0]      0xA5            帧头
//   [1:3]    data_length=300 小端
//   [3]      seq             包序号（每包自增）
//   [4]      CRC8            前 4 字节的 CRC8（0x8C, init 0xFF）
//   [5:7]    0x0310          小端（0x10, 0x03）
//   [7:307]  data            300 字节负载（不足补 0）
//   [307:309] CRC16          前 307 字节的 CRC16（0x1189, init 0xFFFF）
//
// 与自瞄控制帧的区别：控制帧帧头 0xFF（32 字节），视频帧帧头 0xA5（309 字节）。
// 下位机可据此分流。

#ifndef IO__VIDEO_SENDER_HPP
#define IO__VIDEO_SENDER_HPP

#include <cstdint>
#include <memory>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

namespace io
{

// 视频编码参数
struct VideoSenderConfig
{
  int width = 300;              // 输出宽（协议带宽限制）
  int height = 240;             // 输出高：300×240 = 5:4，与相机 1280×1024 完全一致，
                                // 全视野缩放时不留黑边（设 300×300 的话上下黑边会白占 20% 码率）
  bool keep_aspect = true;      // true：整幅画面缩放到 width×height（保比例，不留黑边即全视野）
                                // false：从原图中心裁 crop_size×crop_size 再缩放
  double fps = 10.0;            // 帧率（12→10，同带宽下每帧比特数 +20%）
  int bitrate_kbps = 90;        // 码率（通道上限 40Hz×296B≈94.7kbps，留一点余量防丢包）
  int keyframe_interval = 20;   // 关键帧间隔（帧）
  std::string x264_preset = "slow";  // 画质明显好于 veryfast；实测每帧约 66ms，已放到独立线程编码
  int crop_size = 300;          // keep_aspect=false 时从原图中心裁多少像素
  double sharpen = 0.6;         // USM 锐化强度（0=关闭；提升主观清晰度，几乎不加码率）
  double send_rate_hz = 40.0;   // 出包节拍（包/秒）。协议上限 50Hz；
                                // 排查链路时调到 2Hz，每秒只发 2 包，便于计数对比
  // ---- 「一帧一个包」模式（2026-10-23 新增）----
  // 链路实测通过率只有 0.16%~0.64%，一个多包的大帧几乎不可能完整到达。
  // 打开后：每帧单独编码（全 I 帧）、硬性限制在 296 字节以内，
  //         塞不进一个包就整帧丢掉（绝不拆成多个包），这样每个到达的包都能独立解出一张图。
  bool single_packet = false;
  int max_frame_bytes = 296;    // 一帧的 H.264 上限（300 - 2B长度 - 2B序号 = 296）
};

// 0x0310 帧常量
constexpr std::size_t kVideoCmdDataBytes = 300;
constexpr std::size_t kVideoFrameHeaderBytes = 7;   // A5 + len(2) + seq + crc8 + cmd(2)
constexpr std::size_t kVideoFrameTrailerBytes = 2;  // CRC16
constexpr std::size_t kVideoFrameBytes =
  kVideoFrameHeaderBytes + kVideoCmdDataBytes + kVideoFrameTrailerBytes;  // 309

class VideoSender
{
public:
  explicit VideoSender(const VideoSenderConfig & config = {});
  ~VideoSender();

  VideoSender(const VideoSender &) = delete;
  VideoSender & operator=(const VideoSender &) = delete;

  // 输入 BGR 图像（原图任意尺寸；keep_aspect=true 时整幅缩放，否则中心裁剪后缩放）
  // feed() 只把最新帧交给编码线程（不阻塞），并返回本轮「到点」的 0x0310 帧。
  // 返回可能为空（40Hz 令牌未到）；flush() 会忽略节拍把剩余包全部放出。
  std::vector<std::vector<uint8_t>> feed(
    const cv::Mat & bgr_frame,
    std::int64_t timestamp_ns);

  // 强制刷出编码器内部缓存的帧（停止时调用）
  std::vector<std::vector<uint8_t>> flush();

  const VideoSenderConfig & config() const { return config_; }


private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  VideoSenderConfig config_;

  // make_transport_packet 把一段 H.264 数据封成 300 字节 transport packet，
  // 再包进 309 字节的 0x0310 帧。
  // 内层结构：[2B 长度 BE][2B 序号 BE][H.264 数据]，剩余补 0
  std::vector<std::uint8_t> make_transport_packet(
    const std::uint8_t * payload, std::size_t payload_size);

  // drain_due 按 40Hz 令牌桶（与 ACE 编码端一致）把排队中的包放出来。
  // force=true 时（flush）忽略节拍全部放出。线程安全（内部加锁）。
  void drain_due(std::vector<std::vector<std::uint8_t>> & out, bool force);

  // 工作线程：等待最新帧 → 取景/锐化/编码/封包
  void worker_loop();
  void encode_and_pack(const cv::Mat & bgr_frame);
};

}  // namespace io

#endif  // IO__VIDEO_SENDER_HPP
