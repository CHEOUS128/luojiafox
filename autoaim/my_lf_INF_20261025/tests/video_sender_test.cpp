// video_sender_test.cpp —— VideoSender 离线/在线验证
//
// 两种模式：
//   1. 合成图模式（默认）：程序生成测试画面，不接相机
//   2. 真实相机模式（-c）：读大恒相机，走完整链路
//
// 用法：
//   ./video_sender_test [输出目录]                         # 合成测试图
//   ./video_sender_test [输出目录] -c <config.yaml>        # 真实相机，读配置里的 video_* 参数
//   ./video_sender_test [输出目录] -c <config.yaml> --compare
//                                                          # 另存取景+锐化后画面，便于对比
//
// 配置优先级：命令行覆盖 > config yaml 的 video_* 项 > 内置默认值
// 可用的覆盖项：--fps N --kbps N --preset NAME --keep-aspect 0|1 --crop N --sharpen X
// 想贴 ACE 的取景（中心裁 1000×1000 再缩到 300×240）：--keep-aspect 0 --crop 1000
//
// 输出文件：
//   video_test_frames.bin   完整的 0x0310 帧序列（用于 verify_frames.py 校验）
//   video_test_stream.h264  纯 H.264 码流（用于 ffplay）
//   video_test_last.png     最后一帧原始图像（原始分辨率）
//   video_test_view.png     取景+锐化后的 300×240 画面（--compare 时输出）
//   video_test_encoded.png  解码回来的第一帧（--compare 时用 ffmpeg 生成）
//
// 注意：只把「内层 H.264 载荷」写进 .h264，不把补 0 写进去；
//       补 0 只在整条流末尾一次性裁掉，绝不在中间包上裁。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
#include "tools/yaml.hpp"

namespace
{

constexpr std::size_t kHeaderBytes = 7;    // 0xA5 + len(2) + seq + crc8 + cmd(2)
constexpr std::size_t kDataBytes = 300;
constexpr std::size_t kTrailerBytes = 2;   // CRC16
constexpr std::size_t kTransportPrefix = 2;
constexpr std::size_t kInnerSeqBytes = 2;

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

// 从 309 字节 0x0310 帧中按客户端规则解出内层 H.264 载荷：
//   data 字段 = [2B 内层长度 BE][2B 序号 BE][H.264 ...]，剩余补 0。
// 返回空表示该包没有有效载荷。
std::vector<uint8_t> extractPayload(const std::vector<uint8_t> & frame)
{
  std::vector<uint8_t> payload;
  if (frame.size() != kHeaderBytes + kDataBytes + kTrailerBytes) return payload;
  if (frame[0] != 0xA5) return payload;

  const uint8_t * tp = frame.data() + kHeaderBytes;
  std::size_t off = 0;
  while (off + kTransportPrefix <= kDataBytes) {
    const std::size_t inner_len =
      (static_cast<std::size_t>(tp[off]) << 8) | static_cast<std::size_t>(tp[off + 1]);
    off += kTransportPrefix;
    if (inner_len == 0) break;  // 填充标记
    if (off + inner_len > kDataBytes) break;
    if (inner_len > kInnerSeqBytes) {
      payload.insert(payload.end(), tp + off + kInnerSeqBytes, tp + off + inner_len);
    }
    off += inner_len;
  }
  return payload;
}

// 复刻 VideoSender::feed 的取景+锐化，仅用于 --compare 出图
cv::Mat makeView(const cv::Mat & img, const io::VideoSenderConfig & cfg)
{
  cv::Mat view(cfg.height, cfg.width, CV_8UC3, cv::Scalar(0, 0, 0));
  if (cfg.keep_aspect) {
    const double scale = std::min(static_cast<double>(cfg.width) / img.cols,
                                  static_cast<double>(cfg.height) / img.rows);
    const int nw = std::max(1, static_cast<int>(std::lround(img.cols * scale)));
    const int nh = std::max(1, static_cast<int>(std::lround(img.rows * scale)));
    cv::Mat resized;
    cv::resize(img, resized, cv::Size(nw, nh), 0, 0, cv::INTER_AREA);
    resized.copyTo(view(cv::Rect((cfg.width - nw) / 2, (cfg.height - nh) / 2, nw, nh)));
  } else {
    cv::Mat cropped = img;
    if (img.cols > cfg.crop_size || img.rows > cfg.crop_size) {
      const int x = (img.cols - cfg.crop_size) / 2;
      const int y = (img.rows - cfg.crop_size) / 2;
      cropped = img(cv::Rect(x, y, cfg.crop_size, cfg.crop_size));
    }
    cv::resize(cropped, view, cv::Size(cfg.width, cfg.height), 0, 0, cv::INTER_AREA);
  }
  if (cfg.sharpen > 0.0) {
    cv::Mat blurred;
    cv::GaussianBlur(view, blurred, cv::Size(0, 0), 1.2);
    cv::addWeighted(view, 1.0 + cfg.sharpen, blurred, -cfg.sharpen, 0, view);
  }
  return view;
}

struct CliOverrides
{
  bool has_fps = false;   double fps = 0.0;
  bool has_kbps = false;  int kbps = 0;
  bool has_preset = false; std::string preset;
  bool has_keep_aspect = false; bool keep_aspect = true;
  bool has_crop = false;  int crop = 0;
  bool has_sharpen = false; double sharpen = 0.0;
  bool compare = false;
};

}  // namespace

int main(int argc, char ** argv)
{
  // ---- 参数解析 ----
  std::string out_dir = ".";
  std::string camera_config;  // 非空 = 真实相机模式
  CliOverrides ov;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char * name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "参数 " << name << " 缺少值\n";
        std::exit(1);
      }
      return argv[++i];
    };
    if (a == "-c") {
      camera_config = next("-c");
    } else if (a == "-h" || a == "--help") {
      std::cout << "用法:\n"
                << "  " << argv[0] << " [输出目录]\n"
                << "  " << argv[0] << " [输出目录] -c <config.yaml>\n"
                << "  可选: --fps N --kbps N --preset NAME --keep-aspect 0|1 --crop N --sharpen X --compare\n";
      return 0;
    } else if (a == "--fps") {
      ov.has_fps = true; ov.fps = std::stod(next("--fps"));
    } else if (a == "--kbps") {
      ov.has_kbps = true; ov.kbps = std::stoi(next("--kbps"));
    } else if (a == "--preset") {
      ov.has_preset = true; ov.preset = next("--preset");
    } else if (a == "--keep-aspect") {
      ov.has_keep_aspect = true; ov.keep_aspect = (next("--keep-aspect") != "0");
    } else if (a == "--crop") {
      ov.has_crop = true; ov.crop = std::stoi(next("--crop"));
    } else if (a == "--sharpen") {
      ov.has_sharpen = true; ov.sharpen = std::stod(next("--sharpen"));
    } else if (a == "--compare") {
      ov.compare = true;
    } else {
      out_dir = a;
    }
  }

  const std::string frames_path = out_dir + "/video_test_frames.bin";
  const std::string stream_path = out_dir + "/video_test_stream.h264";
  const std::string last_png = out_dir + "/video_test_last.png";
  const std::string view_png = out_dir + "/video_test_view.png";
  const std::string enc_png = out_dir + "/video_test_encoded.png";

  std::cout << "=== VideoSender 验证 ===\n";
  std::cout << "模式: " << (camera_config.empty() ? "合成测试图" : "真实相机 (" + camera_config + ")")
            << "\n";
  std::cout << "输出目录: " << out_dir << "\n\n";

  // ---- 参数：默认值 < config yaml < 命令行 ----
  io::VideoSenderConfig cfg;  // 默认值见 video_sender.hpp

  if (!camera_config.empty()) {
    try {
      auto yaml = tools::load(camera_config);
      if (yaml["video_width"])          cfg.width = yaml["video_width"].as<int>();
      if (yaml["video_height"])         cfg.height = yaml["video_height"].as<int>();
      if (yaml["video_fps"])            cfg.fps = yaml["video_fps"].as<double>();
      if (yaml["video_bitrate_kbps"])   cfg.bitrate_kbps = yaml["video_bitrate_kbps"].as<int>();
      if (yaml["video_crop_size"])      cfg.crop_size = yaml["video_crop_size"].as<int>();
      if (yaml["video_x264_preset"])    cfg.x264_preset = yaml["video_x264_preset"].as<std::string>();
      if (yaml["video_keep_aspect"])    cfg.keep_aspect = yaml["video_keep_aspect"].as<bool>();
      if (yaml["video_sharpen"])        cfg.sharpen = yaml["video_sharpen"].as<double>();
    } catch (const std::exception & e) {
      std::cerr << "⚠ 读取配置失败（使用默认参数）: " << e.what() << "\n";
    }
  }
  if (ov.has_fps) cfg.fps = ov.fps;
  if (ov.has_kbps) cfg.bitrate_kbps = ov.kbps;
  if (ov.has_preset) cfg.x264_preset = ov.preset;
  if (ov.has_keep_aspect) cfg.keep_aspect = ov.keep_aspect;
  if (ov.has_crop) cfg.crop_size = ov.crop;
  if (ov.has_sharpen) cfg.sharpen = ov.sharpen;

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
  std::cout << "创建 VideoSender...\n";
  io::VideoSender sender(cfg);
  std::cout << "✅ VideoSender 创建成功\n\n";

  std::ofstream frames_out(frames_path, std::ios::binary);
  std::ofstream stream_out(stream_path, std::ios::binary);
  if (!frames_out || !stream_out) {
    std::cerr << "❌ 无法创建输出文件\n";
    return 1;
  }

  // 采样：按时间跑（默认 6 秒），输入帧数随 fps 变化
  constexpr double kRunSeconds = 6.0;
  constexpr int kSrcW = 1280;
  constexpr int kSrcH = 1024;
  const int kTotalFrames = std::max(1, static_cast<int>(std::lround(kRunSeconds * (cfg.fps + 2.0))));

  size_t total_packets = 0;
  size_t total_bytes = 0;
  int frames_with_output = 0;
  std::vector<uint8_t> h264_stream;

  // 各环节耗时统计（帮助判断瓶颈在相机、编码还是节流）
  double t_read = 0, t_compare = 0, t_feed = 0;

  std::cout << "采集 " << kTotalFrames << " 帧（" << (camera ? "真实相机" : "合成 1280x1024")
            << " → " << (cfg.keep_aspect ? "整幅缩放" : "中心裁剪") << " "
            << cfg.width << "x" << cfg.height << " @ " << cfg.fps << "fps "
            << cfg.bitrate_kbps << "kbps preset=" << cfg.x264_preset
            << " sharpen=" << cfg.sharpen << "）...\n";

  cv::Mat last_frame;
  cv::Mat last_view;
  const auto t_start = std::chrono::steady_clock::now();

  // 输入源只需略快于设定 fps；实际编码节流在 VideoSender::feed 里
  const auto sleep_ms = static_cast<int>(std::max(1.0, 1000.0 / (cfg.fps + 2.0)));

  for (int i = 0; i < kTotalFrames; ++i) {
    cv::Mat img;
    const auto t_iter = std::chrono::steady_clock::now();
    if (camera) {
      std::chrono::steady_clock::time_point ts;
      camera->read(img, ts);
      if (img.empty()) {
        std::cerr << "⚠ 第 " << i << " 帧为空，跳过\n";
        continue;
      }
      t_read += std::chrono::duration<double>(std::chrono::steady_clock::now() - t_iter).count();
    } else {
      img = makeTestFrame(i, kSrcW, kSrcH);
    }
    last_frame = img.clone();

    if (ov.compare) {
      const auto t_c = std::chrono::steady_clock::now();
      last_view = makeView(img, cfg);
      t_compare += std::chrono::duration<double>(std::chrono::steady_clock::now() - t_c).count();
    }

    const auto ts_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    const auto t_f = std::chrono::steady_clock::now();
    const auto packets = sender.feed(img, ts_ns);
    t_feed += std::chrono::duration<double>(std::chrono::steady_clock::now() - t_f).count();
    if (!packets.empty()) ++frames_with_output;

    for (const auto & pkt : packets) {
      frames_out.write(reinterpret_cast<const char *>(pkt.data()),
                       static_cast<std::streamsize>(pkt.size()));
      ++total_packets;
      total_bytes += pkt.size();
      const auto payload = extractPayload(pkt);
      h264_stream.insert(h264_stream.end(), payload.begin(), payload.end());
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
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
  if (ov.compare && !last_view.empty()) {
    cv::imwrite(view_png, last_view);
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
  std::cout << "耗时分布(均值):   相机读 " << (t_read * 1000.0 / kTotalFrames) << " ms"
            << " | feed(不阻塞) " << (t_feed * 1000.0 / kTotalFrames) << " ms";
  if (ov.compare) std::cout << " | 对比图 " << (t_compare * 1000.0 / kTotalFrames) << " ms";
  std::cout << "\n";
  std::cout << "注：编码在 VideoSender 独立线程里，feed 只入队最新帧，不应占用主循环时间。\n";

  // 帧数一致性检查
  std::cout << "\n=== 一致性检查 ===\n";
  const size_t expect_bytes = total_packets * 309;
  std::cout << "包数 × 309 = " << expect_bytes << "  实际 = " << total_bytes << "  "
            << (expect_bytes == total_bytes ? "✅" : "❌") << "\n";

  std::cout << "\n输出文件:\n";
  std::cout << "  " << frames_path << "\n";
  std::cout << "  " << stream_path << "\n";
  if (!last_frame.empty()) std::cout << "  " << last_png << "  (原始画面)\n";
  if (ov.compare && !last_view.empty()) std::cout << "  " << view_png << "  (取景+锐化后)\n";
  std::cout << "\n查看:\n";
  std::cout << "  ffplay " << stream_path << "\n";
  std::cout << "  python3 tests/verify_frames.py " << frames_path << " tools/crc.cpp\n";
  if (ov.compare) {
    std::cout << "  ffmpeg -y -v error -i " << stream_path
              << " -frames:v 1 -update 1 " << enc_png << "\n";
    std::cout << "  （对比 " << last_png << " / " << view_png << " / " << enc_png << "）\n";
  }
  return 0;
}
