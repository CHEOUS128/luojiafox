// video_sender.hpp —— 自瞄工业相机画面 → 0x0310 视频帧
//
// 作用：把自瞄主循环读到的 BGR 图像，裁成 300×300、H.264 编码、
//       切成 300 字节定长包，再包成官方 0x0310 图传帧（309 字节），
//       通过 USB CDC 串口发给下位机。
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

// 视频编码参数（默认值即 ACE 实战验证过的配置）
struct VideoSenderConfig
{
  int width = 300;              // 输出宽（协议带宽限制）
  int height = 300;             // 输出高
  double fps = 12.0;            // 帧率
  int bitrate_kbps = 80;        // 码率
  int keyframe_interval = 24;   // 关键帧间隔（帧）
  std::string x264_preset = "veryfast";  // Jetson 上用 veryfast 省 CPU
  int crop_size = 300;          // 从原图中心裁多少（原图 1280×1024 时裁 300×300）
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

  // 输入 BGR 图像（原图任意尺寸，内部会中心裁剪到 crop_size 再缩放到 width×height）
  // 返回该帧生成的所有 0x0310 帧（每帧最多 300 字节负载，可能需要多包）
  // 若本帧不满足发送节流（未到 fps 间隔），返回空 vector
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

  // make_frame 把 300 字节负载包成 309 字节的 0x0310 帧
  std::vector<std::uint8_t> make_frame(
    const std::uint8_t * payload, std::size_t payload_size);
};

}  // namespace io

#endif  // IO__VIDEO_SENDER_HPP
