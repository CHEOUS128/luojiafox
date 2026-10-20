// video_sender_test.cpp —— VideoSender 离线验证
//
// 作用：不接相机、不接串口，用程序生成的测试图验证：
//   1. H.264 编码器正常工作
//   2. 0x0310 帧格式正确（309 字节）
//   3. 输出的码流能被 ffmpeg 解码
//
// 用法：
//   ./video_sender_test [输出目录]
//
// 输出文件：
//   video_test_frames.bin   完整的 0x0310 帧序列（用于校验格式）
//   video_test_stream.h264  纯 H.264 码流（用于 ffplay 播放）

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <string>
#include <thread>
#include <vector>

#include "io/video_sender/video_sender.hpp"

namespace
{

// 生成一帧测试图：渐变背景 + 移动的方块 + 帧号
cv::Mat makeTestFrame(int index, int width, int height)
{
  cv::Mat img(height, width, CV_8UC3);

  // 渐变背景（随时间缓慢变化）
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int r = (x + index * 3) % 256;
      const int g = (y + index * 2) % 256;
      const int b = (x + y + index) % 256;
      img.at<cv::Vec3b>(y, x) = cv::Vec3b(
        static_cast<uchar>(b), static_cast<uchar>(g), static_cast<uchar>(r));
    }
  }

  // 移动的白色方块（方便肉眼判断是否在动）
  const int box_size = 80;
  const int cx = (index * 12) % (width - box_size);
  const int cy = height / 2 - box_size / 2;
  cv::rectangle(img, cv::Rect(cx, cy, box_size, box_size), cv::Scalar(255, 255, 255), -1);

  // 帧号文字
  cv::putText(img, "FRAME " + std::to_string(index), cv::Point(20, 50),
              cv::FONT_HERSHEY_SIMPLEX, 1.2, cv::Scalar(0, 255, 0), 3);

  return img;
}

// 从 0x0310 帧里提取 H.264 payload（只跳过 7 字节头，不裁尾部）
// 注意：中间包 300 字节全是真实数据，绝不能裁；只有整条流最后的补 0 才需要去掉
std::vector<uint8_t> extractPayload(const std::vector<uint8_t> & frame)
{
  constexpr size_t kHeaderBytes = 7;
  constexpr size_t kDataBytes = 300;
  if (frame.size() != kHeaderBytes + kDataBytes + 2) {
    return {};
  }
  return std::vector<uint8_t>(
    frame.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes),
    frame.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes + kDataBytes));
}

}  // namespace

int main(int argc, char ** argv)
{
  const std::string out_dir = (argc > 1) ? argv[1] : ".";
  const std::string frames_path = out_dir + "/video_test_frames.bin";
  const std::string stream_path = out_dir + "/video_test_stream.h264";

  std::cout << "=== VideoSender 离线验证 ===\n";
  std::cout << "输出目录: " << out_dir << "\n\n";

  // 配置：与原图尺寸一致（1280x1024），裁剪 300x300
  io::VideoSenderConfig cfg;
  cfg.width = 300;
  cfg.height = 300;
  cfg.fps = 12.0;
  cfg.bitrate_kbps = 80;
  cfg.keyframe_interval = 24;
  cfg.crop_size = 300;
  cfg.x264_preset = "veryfast";

  std::cout << "创建 VideoSender...\n";
  io::VideoSender sender(cfg);
  std::cout << "✅ VideoSender 创建成功\n\n";

  std::ofstream frames_out(frames_path, std::ios::binary);
  std::ofstream stream_out(stream_path, std::ios::binary);
  if (!frames_out || !stream_out) {
    std::cerr << "❌ 无法创建输出文件\n";
    return 1;
  }

  // 生成 60 帧（12fps 下相当于 5 秒）
  constexpr int kTotalFrames = 60;
  const int src_w = 1280;
  const int src_h = 1024;

  size_t total_packets = 0;
  size_t total_bytes = 0;
  size_t total_h264_bytes = 0;
  int frames_with_output = 0;
  std::vector<uint8_t> h264_stream;   // 先全部攒起来，最后统一裁尾部补 0

  std::cout << "生成 " << kTotalFrames << " 帧测试画面（" << src_w << "x" << src_h
            << " → 裁剪 300x300）...\n";

  const auto t_start = std::chrono::steady_clock::now();

  for (int i = 0; i < kTotalFrames; ++i) {
    cv::Mat img = makeTestFrame(i, src_w, src_h);
    const auto ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();

    const auto packets = sender.feed(img, ts_ns);

    if (!packets.empty()) {
      ++frames_with_output;
    }

    for (const auto & pkt : packets) {
      // 写入完整 0x0310 帧（用于格式校验）
      frames_out.write(reinterpret_cast<const char *>(pkt.data()),
                       static_cast<std::streamsize>(pkt.size()));
      ++total_packets;
      total_bytes += pkt.size();

      // 累加纯 H.264 码流（暂不写文件，最后统一裁尾部补 0）
      const auto payload = extractPayload(pkt);
      h264_stream.insert(h264_stream.end(), payload.begin(), payload.end());
    }

    // 模拟真实帧率（12fps → 83ms 间隔），确保节流逻辑生效
    std::this_thread::sleep_for(std::chrono::milliseconds(83));
  }

  // 刷出编码器缓存
  const auto tail_packets = sender.flush();
  for (const auto & pkt : tail_packets) {
    frames_out.write(reinterpret_cast<const char *>(pkt.data()),
                     static_cast<std::streamsize>(pkt.size()));
    ++total_packets;
    total_bytes += pkt.size();
    const auto payload = extractPayload(pkt);
    h264_stream.insert(h264_stream.end(), payload.begin(), payload.end());
  }

  // 只有整条流最末尾才是补 0，这里统一裁掉
  while (!h264_stream.empty() && h264_stream.back() == 0) {
    h264_stream.pop_back();
  }
  total_h264_bytes = h264_stream.size();
  if (!h264_stream.empty()) {
    stream_out.write(reinterpret_cast<const char *>(h264_stream.data()),
                     static_cast<std::streamsize>(h264_stream.size()));
  }

  const auto t_end = std::chrono::steady_clock::now();
  const double elapsed_s =
    std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count() / 1000.0;

  frames_out.close();
  stream_out.close();

  // ---- 结果汇总 ----
  std::cout << "\n=== 结果 ===\n";
  std::cout << "输入帧数:         " << kTotalFrames << "\n";
  std::cout << "有输出的帧:       " << frames_with_output << "\n";
  std::cout << "0x0310 包数:      " << total_packets << "\n";
  std::cout << "0x0310 总字节:    " << total_bytes << " (" << total_bytes / 1024.0 << " KB)\n";
  std::cout << "H.264 总字节:     " << total_h264_bytes << " (" << total_h264_bytes / 1024.0 << " KB)\n";
  std::cout << "耗时:             " << elapsed_s << " 秒\n";
  std::cout << "实际帧率:         " << (frames_with_output / elapsed_s) << " fps\n";
  std::cout << "平均码率:         " << (total_h264_bytes * 8.0 / elapsed_s / 1000.0) << " kbps\n";
  std::cout << "\n输出文件:\n";
  std::cout << "  " << frames_path << "  (0x0310 帧序列)\n";
  std::cout << "  " << stream_path << "  (H.264 裸流)\n";
  std::cout << "\n验证命令:\n";
  std::cout << "  ffprobe " << stream_path << "\n";
  std::cout << "  ffplay " << stream_path << "   # 应该看到视频在动\n";
  std::cout << "  python3 verify_frames.py " << frames_path << "\n";

  return 0;
}
