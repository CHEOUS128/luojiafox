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
//               --large-frame 0|1 --gop N --gray 0|1 --seconds S
// 想贴 ACE 的取景（中心裁 1000×1000 再缩到 300×240）：--keep-aspect 0 --crop 1000
//
// 输出文件：
//   video_test_frames.bin   完整的 0x0310 帧序列（用于 verify_frames.py 校验）
//   video_test_stream.h264  纯 H.264 码流（用于 ffplay）
//   video_test_last.png     最后一帧原始图像（原始分辨率）
//   video_test_view.png     取景+锐化后的 300×240 画面（--compare 时输出）
//   video_test_encoded.png  解码回来的第一帧（--compare 时用 ffmpeg 生成）
//   video_test_raw.bin      每帧**相机原片**，紧排 BGR（W×H×3 字节/帧）
//                           W/H 就是相机真实出图尺寸，本程序在启动日志里打出来。
//
// ★ 2026-11-07 CH 的规矩：左栏素材必须是车上这枚相机拍的**彩色原片**，
//   不能在 Windows 上从测试画面凑。所以 -c 模式下把每一帧原图原封不动写出来，
//   拷回 Windows 交给 scripts\准备素材.bat 当编码输入，左栏也用同一份。
//   体积：1280×1024×3 ≈ 3.75 MB/帧。--seconds 控制时长（默认 6 秒 ≈ 450 MB）。
//
// 注意：只把「内层 H.264 载荷」写进 .h264，不把补 0 写进去；
//       补 0 只在整条流末尾一次性裁掉，绝不在中间包上裁。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <opencv2/opencv.hpp>
#include <string>
#include <thread>
#include <vector>

#include "io/camera.hpp"
#include "io/video_sender/video_sender.hpp"
#include "io/video_sender/cw_tx_worker.hpp"
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
  if (!cfg.cw_profile.empty()) return io::VideoSender::trial_view(img,cfg);
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
  std::string cw_profile;
  bool has_fps = false;   double fps = 0.0;
  bool has_kbps = false;  int kbps = 0;
  bool has_preset = false; std::string preset;
  bool has_keep_aspect = false; bool keep_aspect = true;
  bool has_crop = false;  int crop = 0;
  bool has_sharpen = false; double sharpen = 0.0;
  bool has_large_frame = false; bool large_frame = false;
  bool has_gop = false;   int gop = 5;
  bool has_gray = false;  bool gray = false;
  bool has_seconds = false; double seconds = 6.0;
  bool compare = false;
};

// 统计「一帧拆成几个包」的分布。
// 判据：一个内层包如果载荷正好填满 296 字节（300-2B长度-2B序号），说明这一帧还没完；
//       否则（短包）就是本帧最后一包。H.264 末尾必有 rbsp_stop_bit，不可能刚好整包填满，
//       所以这条判据在实测中是安全的。I+P 模式下这一行能直接看出 I 帧是不是 3 包、P 帧是不是 1 包。
struct FrameRunStats
{
  std::map<std::size_t, std::size_t> hist;  // 包数 → 帧数
  std::size_t frames = 0;
  std::size_t packets = 0;
  std::size_t cur = 0;

  void add(const std::vector<uint8_t> & frame, std::vector<uint8_t> & h264_out)
  {
    const auto payload = extractPayload(frame);
    const bool full_packet = payload.size() >= (kDataBytes - kTransportPrefix - kInnerSeqBytes);
    h264_out.insert(h264_out.end(), payload.begin(), payload.end());
    ++packets;
    ++cur;
    if (!full_packet) {
      ++hist[cur];
      ++frames;
      cur = 0;
    }
  }

  void finish()
  {
    if (cur > 0) {  // 最后一帧被 flush 截断（少见）
      ++hist[cur];
      ++frames;
      cur = 0;
    }
  }

  void print(const char * title) const
  {
    std::cout << title << "：共 " << frames << " 帧 / " << packets << " 包  ";
    for (const auto & [n, c] : hist) std::cout << "[" << n << "包 × " << c << "帧] ";
    std::cout << "\n";
  }
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
                << "  可选: --fps N --kbps N --preset NAME --keep-aspect 0|1 --crop N --sharpen X\n"
                << "        --large-frame 0|1 --gop N --gray 0|1 --seconds S --compare\n"
                << "        --cw-profile baseline|smooth|detail|detail-aq0|quality (cw-trial-20261109-r2)\n";
      return 0;
    } else if (a == "--cw-profile") {
      ov.cw_profile = next("--cw-profile");
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
    } else if (a == "--large-frame") {
      ov.has_large_frame = true; ov.large_frame = (next("--large-frame") != "0");
    } else if (a == "--gop") {
      ov.has_gop = true; ov.gop = std::stoi(next("--gop"));
    } else if (a == "--gray") {
      ov.has_gray = true; ov.gray = (next("--gray") != "0");
    } else if (a == "--seconds") {
      ov.has_seconds = true; ov.seconds = std::stod(next("--seconds"));
    } else if (a == "--compare") {
      ov.compare = true;
    } else if (!a.empty() && a[0] == '-') {
      // 以前不认识的参数会掉进 out_dir 分支，变成「输出目录: --seconds」这种静默错乱。
      std::cerr << "❌ 不认识的参数: " << a << "（用 --help 看用法）\n";
      return 1;
    } else {
      out_dir = a;
    }
  }

  const std::string frames_path = out_dir + "/video_test_frames.bin";
  const std::string stream_path = out_dir + "/video_test_stream.h264";
  const std::string last_png = out_dir + "/video_test_last.png";
  const std::string view_png = out_dir + "/video_test_view.png";
  const std::string enc_png = out_dir + "/video_test_encoded.png";
  // ★ 相机原片：压片战争的左栏素材 + 编码输入都从这一个文件来。
  const std::string orig_raw = out_dir + "/video_test_raw.bin";

  // 输出目录不存在就建（records/raw 这种路径，以前只能手动 mkdir，还会撞权限）
  {
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (!std::filesystem::is_directory(out_dir, ec))
      std::cerr << "⚠ 建不出输出目录 " << out_dir << "（" << ec.message()
                << "），文件可能写不进去\n";
  }

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
      if (yaml["video_send_rate_hz"])   cfg.send_rate_hz = yaml["video_send_rate_hz"].as<double>();
      if (yaml["video_single_packet"])  cfg.single_packet = yaml["video_single_packet"].as<bool>();
      if (yaml["video_max_frame_bytes"]) cfg.max_frame_bytes = yaml["video_max_frame_bytes"].as<int>();
      // ---- 2026-10-28 画质版：I+P + 一帧拆多包（与 standard_mpc 的读取保持一致）----
      if (yaml["video_large_frame"])    cfg.large_frame = yaml["video_large_frame"].as<bool>();
      if (yaml["video_gop"])            cfg.gop = yaml["video_gop"].as<int>();
      if (yaml["video_max_frame_packets"]) cfg.max_frame_packets = yaml["video_max_frame_packets"].as<int>();
      if (yaml["video_gray"])           cfg.gray = yaml["video_gray"].as<bool>();
      if (cfg.large_frame) cfg.single_packet = false;  // 互斥，大帧优先
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
  if (ov.has_large_frame) cfg.large_frame = ov.large_frame;
  if (ov.has_gop) cfg.gop = ov.gop;
  if (ov.has_gray) cfg.gray = ov.gray;
  if (cfg.large_frame) cfg.single_packet = false;  // 命令行覆盖之后也要保证互斥

  if (!ov.cw_profile.empty()) {
    if (!io::cw::valid_profile(ov.cw_profile)) { std::cerr << "Unknown cw profile\n"; return 2; }
    io::cw::apply(cfg, ov.cw_profile);
  }

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

  // ★ 原片输出：按 video_fps 节流，和 VideoSender 内部接受编码的节拍**完全一样**，
  //   这样 raw 的第 i 帧 ≈ 编码器的第 i 帧，左栏和右栏才配得上。
  //   必须照抄 VideoSender::feed 的「固定节拍累加」，不能用 now-last>=interval，
  //   否则节拍会漂，120 帧能漂出好几帧的错位。
  //   必须是 -c 真实相机模式才有意义（合成图模式下写出来的是假画面）。
  auto raw_next = std::chrono::steady_clock::now();
  const auto raw_interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
    std::chrono::duration<double>(1.0 / std::max(0.1, cfg.fps)));
  std::ofstream orig_raw_out(orig_raw, std::ios::binary);
  if (!orig_raw_out) {
    std::cerr << "❌ 无法创建 " << orig_raw << "（磁盘满了？）\n";
    return 1;
  }
  int orig_saved = 0;      // 写进 video_test_raw.bin 的帧数
  int orig_w = 0;          // 相机真实出图宽（第一帧确定）
  int orig_h = 0;          // 相机真实出图高

  // 采样：按时间跑（默认 6 秒）。输入源模拟真实相机（约 75fps），
  // 由 VideoSender 内部按 video_fps 节流；不再人为 sleep 到 12fps——
  // 那样每轮计时会和 100ms 节流窗口打架，把重复帧全丢掉，帧率统计失真。
  const double kRunSeconds = std::max(1.0, ov.seconds);
  constexpr int kSrcW = 1280;
  constexpr int kSrcH = 1024;
  constexpr double kSourceFps = 75.0;          // 模拟自瞄主循环频率
  const auto loop_period = std::chrono::duration<double>(1.0 / kSourceFps);

  size_t total_packets = 0;
  size_t total_bytes = 0;
  int frames_with_output = 0;
  std::vector<uint8_t> h264_stream;
  FrameRunStats run_stats;
  auto collect = [&](const std::vector<std::vector<uint8_t>> & packets) {
    for (const auto & pkt : packets) {
      frames_out.write(reinterpret_cast<const char *>(pkt.data()),
                       static_cast<std::streamsize>(pkt.size()));
      ++total_packets;
      total_bytes += pkt.size();
      run_stats.add(pkt, h264_stream);
    }
  };

  std::ofstream cw_times;
  std::size_t cw_remaining = 0, cw_bundle_size = 0, cw_bytes = 0;
  const auto cw_begin = std::chrono::steady_clock::now();
  std::unique_ptr<io::cw::TxWorker> cw_tx;
  if (!cfg.cw_profile.empty()) {
    cw_times.open(out_dir + "/cw-packet-times.csv");
    if (!cw_times) { std::cerr << "cannot create cw-packet-times.csv\n"; return 1; }
    cw_times << "packet,steady_ns,frame,frame_packets,frame_h264_bytes\n";
    cw_tx = std::make_unique<io::cw::TxWorker>(
      [&] {
        auto b = sender.take_trial_frame();
        if (!b.empty()) {
          cw_remaining = cw_bundle_size = b.size(); cw_bytes = 0;
          for (const auto& p : b) cw_bytes += extractPayload(p).size();
        }
        return b;
      },
      [&](const auto& p) {
        const auto at = std::chrono::steady_clock::now();
        frames_out.write(reinterpret_cast<const char*>(p.data()),p.size());
        auto payload = extractPayload(p);
        h264_stream.insert(h264_stream.end(),payload.begin(),payload.end());
        ++total_packets; total_bytes += p.size(); ++run_stats.packets;
        cw_times << total_packets << ',' << std::chrono::duration_cast<std::chrono::nanoseconds>(at-cw_begin).count()
                 << ',' << run_stats.frames << ',' << cw_bundle_size << ',' << cw_bytes << '\n';
        if (--cw_remaining == 0) { ++run_stats.hist[cw_bundle_size]; ++run_stats.frames; }
        if (!frames_out || !cw_times) throw std::runtime_error("cw capture write failed");
        return true;
      }, [] {});
  }

  // 各环节耗时统计（帮助判断瓶颈在相机、编码还是节流）
  double t_read = 0, t_compare = 0, t_feed = 0;

  std::cout << "采集 " << kRunSeconds << " 秒（" << (camera ? "真实相机" : "合成 1280x1024")
            << " → " << (cfg.keep_aspect ? "整幅缩放" : "中心裁剪") << " "
            << " @" << cfg.fps << "fps "
            << cfg.bitrate_kbps << "kbps preset=" << cfg.x264_preset
            << " keep_aspect=" << cfg.keep_aspect
            << " 单包=" << cfg.single_packet << " 大帧=" << cfg.large_frame
            << " GOP=" << cfg.gop << " 灰度=" << cfg.gray
            << " sharpen=" << cfg.sharpen << "）...\n";

  cv::Mat last_view;
  cv::Mat last_input;
  const auto t_start = std::chrono::steady_clock::now();
  const auto deadline_end = t_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
    std::chrono::duration<double>(kRunSeconds));

  int total_input = 0;
  int i = 0;
  for (; std::chrono::steady_clock::now() < deadline_end; ++i) {
    const auto iter_start = std::chrono::steady_clock::now();
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
    ++total_input;

    if (ov.compare) {
      const auto t_c = std::chrono::steady_clock::now();
      last_view = makeView(img, cfg);
      t_compare += std::chrono::duration<double>(std::chrono::steady_clock::now() - t_c).count();
    }

    const auto ts_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    last_input = img;   // 浅拷贝：只用来最后保存原始画面

    // ★ 原片一定要在 feed() 之前存：
    //   encode_and_pack 里的 cv::resize / USM 会改动同一块 Mat 的数据
    //   （它拿的是 const&，但 cv::Mat 浅拷贝共享像素），喂完再存就不是原片了。
    const auto now_raw = std::chrono::steady_clock::now();
    if (!img.empty() && img.type() == CV_8UC3 && now_raw >= raw_next) {
      raw_next += raw_interval;
      if (raw_next + raw_interval < now_raw) raw_next = now_raw + raw_interval;
      if (orig_w == 0) {
        orig_w = img.cols;
        orig_h = img.rows;
        std::cout << "相机实际出图 " << orig_w << "x" << orig_h
                  << "（原片 3 通道 BGR；压片仿真的左栏 = 这个分辨率）\n";
        if (orig_w != kSrcW || orig_h != kSrcH) {
          std::cout << "⚠ 和默认假设 " << kSrcW << "x" << kSrcH
                    << " 不一致；cam_*.bgr 必须按 " << orig_w << "x" << orig_h
                    << " 声明，否则 Windows 那边切帧会错位。本程序最后会打出正确命令。\n";
        }
      }
      if (img.cols == orig_w && img.rows == orig_h) {
        orig_raw_out.write(reinterpret_cast<const char *>(img.data),
                           static_cast<std::streamsize>(img.total() * img.elemSize()));
        if (!orig_raw_out) {
          // 磁盘满/写失败：早点说，否则拷回去的文件在 Windows 那边切帧校验才会发现
          std::cerr << "❌ 写原片失败（磁盘满了？）已停在第 " << orig_saved
                    << " 帧，这个 bin 不完整，别拷\n";
          return 1;
        }
        ++orig_saved;
      }
    }

    const auto t_f = std::chrono::steady_clock::now();
    const auto packets = sender.feed(img, ts_ns);
    t_feed += std::chrono::duration<double>(std::chrono::steady_clock::now() - t_f).count();
    if (!packets.empty()) ++frames_with_output;

    collect(packets);

    const auto iter_elapsed = std::chrono::steady_clock::now() - iter_start;
    if (iter_elapsed < std::chrono::duration_cast<std::chrono::steady_clock::duration>(loop_period)) {
      std::this_thread::sleep_for(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(loop_period) - iter_elapsed);
    }
  }

  // flush 剩余
  if (cw_tx) {
    // Only the already pulled bundle may be unfinished; no forced queue release.
    cw_tx->stop();
    if (cw_tx->failed()) { std::cerr << "CW worker failed; capture invalid\n"; return 1; }
    std::cout << "[CW] end pending bundle packets discarded=" << cw_remaining << "\n";
  } else { collect(sender.flush()); run_stats.finish(); }

  // 只裁整条流末尾补的 0
  while (!h264_stream.empty() && h264_stream.back() == 0) h264_stream.pop_back();
  if (!h264_stream.empty()) {
    stream_out.write(reinterpret_cast<const char *>(h264_stream.data()),
                     static_cast<std::streamsize>(h264_stream.size()));
  }

  // 保存最后一帧原始图（确认相机画面本身正常）
  if (!last_input.empty()) {
    cv::imwrite(last_png, last_input);
  }
  if (ov.compare && !last_view.empty()) {
    cv::imwrite(view_png, last_view);
  }

  const auto t_end = std::chrono::steady_clock::now();
  const double elapsed_s =
    std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count() / 1000.0;
  frames_out.close();
  stream_out.close();
  orig_raw_out.close();

  // ---- 给压片仿真用的原片 ----
  std::cout << "\n=== 压片仿真用的相机原片（2026-11-07）===\n";
  if (camera) {
    std::cout << "原片帧数  " << orig_saved << " 帧  "
              << (orig_w > 0 ? std::to_string(orig_w) + "x" + std::to_string(orig_h) + "x3" : std::string("尺寸未知"))
              << " BGR（原封不动）→ " << orig_raw << "\n";
    if (orig_saved > 0 && orig_w > 0) {
      const long long per_frame = static_cast<long long>(orig_w) * orig_h * 3;
      std::cout << "文件字节  " << (per_frame * orig_saved) << "（" << per_frame << " / 帧）\n";
      std::cout << "拷回 Windows 后跑（把三个 <路径> 换成实际路径）：\n";
      std::cout << "  scripts\\准备素材.bat <这个bin> "
                << orig_w << "x" << orig_h << "\n";
    }
  } else {
    std::cout << "⚠ 合成图模式：原片是程序画出来的，不是相机，已跳过不写。\n"
                 "   要拿真原片请加 -c configs/standard3.yaml\n";
  }

  // ---- 汇总 ----
  std::cout << "\n=== 结果 ===\n";
  std::cout << "输入帧数:         " << total_input << " (" << kRunSeconds << " 秒 @ " << kSourceFps << "fps)\n";
  std::cout << "有输出的帧:       " << frames_with_output << "\n";
  std::cout << "0x0310 包数:      " << total_packets << "\n";
  std::cout << "0x0310 总字节:    " << total_bytes << " (" << total_bytes / 1024.0 << " KB)\n";
  std::cout << "H.264 总字节:     " << h264_stream.size() << " (" << h264_stream.size() / 1024.0
            << " KB)\n";
  std::cout << "耗时:             " << elapsed_s << " 秒\n";
  std::cout << "实际帧率:         " << (frames_with_output / elapsed_s) << " fps\n";
  std::cout << "平均码率:         " << (h264_stream.size() * 8.0 / elapsed_s / 1000.0) << " kbps\n";
  const double n_in = std::max(1, total_input);
  std::cout << "耗时分布(均值):   相机读 " << (t_read * 1000.0 / n_in) << " ms"
            << " | feed(不阻塞) " << (t_feed * 1000.0 / n_in) << " ms";
  if (ov.compare) std::cout << " | 对比图 " << (t_compare * 1000.0 / n_in) << " ms";
  std::cout << "\n";
  // ★ 判断大帧模式是否生效就看这一行：
  //   大帧+I+P 应该是 [1包 × 多数帧] 和 [3包 × 少数帧] 两种，包率约 28/秒；
  //   如果只有 [1包 × 全部]，说明 P 帧/拆分没生效（或 I 帧都被超限丢了）。
  run_stats.print("每帧包数分布");
  std::cout << "包率:             " << (total_packets / elapsed_s) << " 包/秒"
            << "（令牌桶上限 " << cfg.send_rate_hz << "/秒；大帧 GOP" << cfg.gop
            << " 理论约 " << (cfg.fps * (1.0 + (cfg.gop - 1.0) / cfg.gop)) << " 包/秒）\n";
  std::cout << "注：编码在 VideoSender 独立线程里，feed 只入队最新帧，不应占用主循环时间。\n";

  // 帧数一致性检查
  std::cout << "\n=== 一致性检查 ===\n";
  const size_t expect_bytes = total_packets * 309;
  std::cout << "包数 × 309 = " << expect_bytes << "  实际 = " << total_bytes << "  "
            << (expect_bytes == total_bytes ? "✅" : "❌") << "\n";

  std::cout << "\n输出文件:\n";
  std::cout << "  " << frames_path << "\n";
  std::cout << "  " << stream_path << "\n";
  if (!last_input.empty()) std::cout << "  " << last_png << "  (原始画面)\n";
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
