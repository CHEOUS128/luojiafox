// video_sender_test.cpp —— VideoSender 离线/在线验证
//
// 两种模式：
//   1. 合成图模式（默认）：程序生成测试画面，不接相机
//   2. 真实相机模式（-c）：读大恒相机，走完整链路
//
// 用法：
//   ./video_sender_test [输出目录]                  # 合成测试图
//   ./video_sender_test [输出目录] -c <config.yaml> # 真实相机
//
// 输出文件：
//   video_test_frames.bin   完整的 0x0310 帧序列（用于校验格式）
//   video_test_stream.h264  纯 H.264 码流（用于 ffplay）
//   video_test_last.png     最后一帧原始图像（用于确认相机画面正常）

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <opencv2/opencv.hpp>
#include <string>
#include <thread>
#include <vector>

#include "io/camera.hpp"
#include "io/video_sender/video_sender.hpp"

namespace
{

// 生成一帧测试图：渐变背景 + 移动的方块 + 帧号
cv::Mat makeTestFrame(int index, int width, int height)
{
  cv::Mat img(height, width, CV_8UC3);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int r = (x + index * 3) % 256;
      const int g = (y + index * 2) % 256;
      const int b = (x + y + index) % 256;
      img.at<cv::Vec3b>(y, x) =
        cv::Vec3b(static_cast<uchar>(b), static_cast<uchar>(g), static_cast<uchar>(r));
    }
  }
  const int box_size = 80;
  const int cx = (index * 12) % (width - box_size);
  const int cy = height / 2 - box_size / 2;
  cv::rectangle(img, cv::Rect(cx, cy, box_size, box_size), cv::Scalar(255, 255, 255), -1);
  cv::putText(img, "FRAME " + std::to_string(index), cv::Point(20, 50),
              cv::FONT_HERSHEY_SIMPLEX, 1.2, cv::Scalar(0, 255, 0), 3);
  return img;
}

// 从 0x0310 帧里提取 H.264 payload（只跳过 7 字节头，不裁尾部）
std::vector<uint8_t> extractPayload(const std::vector<uint8_t> & frame)
{
  constexpr size_t kHeaderBytes = 7;
  constexpr size_t kDataBytes = 300;
  if (frame.size() != kHeaderBytes + kDataBytes + 2) return {};
  return std::vector<uint8_t>(
    frame.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes),
    frame.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes + kDataBytes));
}

}  // namespace

int main(int argc, char ** argv)
{
  // ---- 参数解析 ----
  std::string out_dir = ".";
  std::string camera_config;  // 非空 = 真实相机模式
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-c" && i + 1 < argc) {
      camera_config = argv[++i];
    } else if (a == "-h" || a == "--help") {
      std::cout << "用法:\n"
                << "  " << argv[0] << " [输出目录]\n"
                << "  " << argv[0] << " [输出目录] -c <config.yaml>   # 真实相机\n";
      return 0;
    } else {
      out_dir = a;
    }
  }

  const std::string frames_path = out_dir + "/video_test_frames.bin";
  const std::string stream_path = out_dir + "/video_test_stream.h264";
  const std::string last_png = out_dir + "/video_test_last.png";

  std::cout << "=== VideoSender 验证 ===\n";
  std::cout << "模式: " << (camera_config.empty() ? "合成测试图" : "真实相机 (" + camera_config + ")")
            << "\n";
  std::cout << "输出目录: " << out_dir << "\n\n";

  // ---- 创建相机（可选）----
  std::unique_ptr<io::Camera> camera;
  if (!camera_config.empty()) {
    std::cout << "打开相机...\n";
    try {
      camera = std::make_unique<io::Camera>(camera_config);
      std::cout << "✅ 相机已打开\n\n";
    } catch (const std::exception & e) {
      std::cerr << "❌ 相机打开失败: " << e.what() << "\n";
      return 1;
    }
  }

  // ---- 创建 VideoSender ----
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

  constexpr int kTotalFrames = 60;
  const int src_w = 1280;
  const int src_h = 1024;

  size_t total_packets = 0;
  size_t total_bytes = 0;
  int frames_with_output = 0;
  std::vector<uint8_t> h264_stream;

  std::cout << "采集 " << kTotalFrames << " 帧（" << src_w << "x" << src_h
            << " → 裁剪 300x300）...\n";

  cv::Mat last_frame;
  const auto t_start = std::chrono::steady_clock::now();

  for (int i = 0; i < kTotalFrames; ++i) {
    cv::Mat img;
    if (camera) {
      std::chrono::steady_clock::time_point ts;
      camera->read(img, ts);
      if (img.empty()) {
        std::cerr << "⚠ 第 " << i << " 帧为空，跳过\n";
        continue;
      }
    } else {
      img = makeTestFrame(i, src_w, src_h);
    }
    last_frame = img.clone();

    const auto ts_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    const auto packets = sender.feed(img, ts_ns);
    if (!packets.empty()) ++frames_with_output;

    for (const auto & pkt : packets) {
      frames_out.write(reinterpret_cast<const char *>(pkt.data()),
                       static_cast<std::streamsize>(pkt.size()));
      ++total_packets;
      total_bytes += pkt.size();
      const auto payload = extractPayload(pkt);
      h264_stream.insert(h264_stream.end(), payload.begin(), payload.end());
    }

    // 节流到 12fps（真实相机可能跑 40~100fps）
    std::this_thread::sleep_for(std::chrono::milliseconds(83));
  }

  // flush 剩余
  for (const auto & pkt : sender.flush()) {
    frames_out.write(reinterpret_cast<const char *>(pkt.data()),
                     static_cast<std::streamsize>(pkt.size()));
    ++total_packets;
    total_bytes += pkt.size();
    const auto payload = extractPayload(pkt);
    h264_stream.insert(h264_stream.end(), payload.begin(), payload.end());
  }

  // 只裁整条流末尾补的 0
  while (!h264_stream.empty() && h264_stream.back() == 0) h264_stream.pop_back();
  if (!h264_stream.empty()) {
    stream_out.write(reinterpret_cast<const char *>(h264_stream.data()),
                     static_cast<std::streamsize>(h264_stream.size()));
  }

  // 保存最后一帧原始图（确认相机画面本身正常）
  if (!last_frame.empty()) {
    cv::imwrite(last_png, last_frame);
  }

  const auto t_end = std::chrono::steady_clock::now();
  const double elapsed_s =
    std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count() / 1000.0;
  frames_out.close();
  stream_out.close();

  // ---- 汇总 ----
  std::cout << "\n=== 结果 ===\n";
  std::cout << "输入帧数:         " << kTotalFrames << "\n";
  std::cout << "有输出的帧:       " << frames_with_output << "\n";
  std::cout << "0x0310 包数:      " << total_packets << "\n";
  std::cout << "0x0310 总字节:    " << total_bytes << " (" << total_bytes / 1024.0 << " KB)\n";
  std::cout << "H.264 总字节:     " << h264_stream.size() << " (" << h264_stream.size() / 1024.0
            << " KB)\n";
  std::cout << "耗时:             " << elapsed_s << " 秒\n";
  std::cout << "实际帧率:         " << (frames_with_output / elapsed_s) << " fps\n";
  std::cout << "平均码率:         " << (h264_stream.size() * 8.0 / elapsed_s / 1000.0) << " kbps\n";

  // 帧数一致性检查
  std::cout << "\n=== 一致性检查 ===\n";
  const size_t expect_bytes = total_packets * 309;
  std::cout << "包数 × 309 = " << expect_bytes << "  实际 = " << total_bytes << "  "
            << (expect_bytes == total_bytes ? "✅" : "❌") << "\n";

  std::cout << "\n输出文件:\n";
  std::cout << "  " << frames_path << "\n";
  std::cout << "  " << stream_path << "\n";
  if (!last_frame.empty()) std::cout << "  " << last_png << "  (原始画面)\n";
  std::cout << "\n查看:\n";
  std::cout << "  ffplay " << stream_path << "\n";
  std::cout << "  python3 tests/verify_frames.py " << frames_path << " tools/crc.cpp\n";

  return 0;
}
