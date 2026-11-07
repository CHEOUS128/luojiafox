// camera_record_test.cpp —— 只读工业相机、把原帧逐张存成 PNG
//
// 为什么单独写一个：压片战争要的是「编码前的真原片」。
//   video_sender_test 只会存最后一帧原图（video_test_last.png），
//   前面那些画质对比是拿编码后的 .h264 回读出来的，已经被 x264 压过一遍，
//   拿来当"清晰原版"是循环论证。
//
// 本程序不碰 CAN / 串口 / 云台 / YOLO，只构造 io::Camera 一种对象，
// 所以没有 C 板时也能跑。
//
// ★ 2026-11-07 CH 定的规矩：相机录下来的是**原始数据，原封不动**。
//   转灰度、缩放都是"我们这边后续处理"，不许作用在原始视频上。
//   所以本程序的默认是**彩色**（相机原生 BGR → PNG），灰度要显式 --gray。
//   （车上 VideoSender 的 video_gray:true 是发送端的处理，在仿真里由右栏体现，
//    不该在录制环节就把颜色丢掉。之前默认灰度是个错。）
//
// 用法：
//   sudo ./build/camera_record_test records/cam_raw              # 200 帧、20fps、彩色（原始）
//   sudo ./build/camera_record_test records/cam_raw -n 400 -fps 20 -c configs/standard3.yaml
//   ./build/camera_record_test records/cam_raw -n 100 --gray     # 存灰度（一般不用）
//
// 输出：<out>/000000.png …（编号 6 位，按录制顺序）
//
// ⚠ -fps 20 是**故意节流**到 20：车上的 VideoSender 也是把相机帧按
//   video_fps=20 节流后才进编码器的。按 75fps 全录再按 20fps 播，
//   运动会被放慢 3.75 倍，仿真里的"相邻帧位移"就比实车小，结论会偏乐观。
//
// ⚠ 为什么不用 cv::CommandLineParser：它把 "{" "}" 当分隔符，帮助文本里
//   只要出现一个花括号（比如写 "（...）" 时手滑写成 "{"），整个 keys 串
//   就直接抛 assert。而且它解析失败是 CUDA 异常、不给行号，很难查。
//   这里手写一个极小的解析，覆盖 5 个参数就够。

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <opencv2/opencv.hpp>

#include "io/camera.hpp"
#include "tools/logger.hpp"

namespace fs = std::filesystem;

namespace {

void print_usage(const char * app)
{
  std::printf(
    "用法: %s [out]\n"
    "  out            输出目录（PNG 逐帧落盘，默认 records/cam_raw，可省略）\n"
    "  -n <frames>    录多少帧（默认 200）\n"
    "  -fps <f>       节流到多少帧每秒（默认 20.0，= 车端 video_fps）\n"
    "  -c <yaml>      相机配置（默认 configs/standard3.yaml，里面 camera_name=daheng）\n"
    "  --color        存彩色 BGR（默认；相机原片就该是彩色）\n"
    "  --gray         存灰度（一般不用：转灰度是发送端的处理，不该写进原片）\n"
    "  -h --help      打印本帮助\n",
    app);
}

bool arg_is(const char * s, const char * a, const char * b)
{
  return std::strcmp(s, a) == 0 || (b && std::strcmp(s, b) == 0);
}

}  // namespace

int main(int argc, char ** argv)
{
  std::string out_dir = "records/cam_raw";
  int want_frames = 200;
  double fps = 20.0;
  std::string config_path = "configs/standard3.yaml";
  bool gray = false;   // ★ 默认彩色：录的是相机原始数据，不做任何处理

  // 第一个不以下划线开头的参数 = 位置参数（输出目录）
  bool out_set = false;

  for (int i = 1; i < argc; ++i)
  {
    const char * s = argv[i];
    if (arg_is(s, "-h", "--help")) {
      print_usage(argv[0]);
      return 0;
    }
    if (std::strcmp(s, "-n") == 0 || std::strcmp(s, "--frames") == 0) {
      if (i + 1 >= argc) { std::fprintf(stderr, "-n 缺少值\n"); return 2; }
      want_frames = std::atoi(argv[++i]);
      continue;
    }
    if (std::strcmp(s, "-fps") == 0) {
      if (i + 1 >= argc) { std::fprintf(stderr, "-fps 缺少值\n"); return 2; }
      fps = std::atof(argv[++i]);
      continue;
    }
    if (std::strcmp(s, "-c") == 0 || std::strcmp(s, "--config") == 0) {
      if (i + 1 >= argc) { std::fprintf(stderr, "-c 缺少值\n"); return 2; }
      config_path = argv[++i];
      continue;
    }
    if (std::strcmp(s, "--color") == 0) { gray = false; continue; }
    if (std::strcmp(s, "--gray") == 0)  { gray = true;  continue; }
    if (s[0] == '-') {
      std::fprintf(stderr, "不认识的参数: %s（用 -h 看用法）\n", s);
      return 2;
    }
    if (!out_set) { out_dir = s; out_set = true; continue; }
    std::fprintf(stderr, "多余的参数: %s\n", s);
    return 2;
  }

  want_frames = std::max(1, want_frames);
  fps = std::max(1.0, fps);

  if (!fs::exists(config_path)) {
    tools::logger()->error("找不到配置文件：{}（要在 autoaim 目录下跑）", config_path);
    return 1;
  }

  fs::create_directories(out_dir);

  tools::logger()->info("工业相机录帧启动");
  tools::logger()->info("输出 {}  目标 {} 帧  {} fps  {}",
                        out_dir, want_frames, fps, gray ? "灰度" : "彩色");

  io::Camera camera(config_path);
  tools::logger()->info("相机已打开");

  const auto period = std::chrono::duration<double>(1.0 / fps);
  const auto t_start = std::chrono::steady_clock::now();
  auto next_due = t_start;
  auto last_kept = t_start;

  int saved = 0;
  int skipped_empty = 0;

  while (saved < want_frames) {
    cv::Mat img;
    std::chrono::steady_clock::time_point ts;
    camera.read(img, ts);

    if (img.empty()) {
      ++skipped_empty;
      continue;
    }

    // 节流：两次落盘之间至少隔 1/fps 秒（和 VideoSender 的 feed() 同一套语义）
    const auto now = std::chrono::steady_clock::now();
    if (now < next_due) continue;

    cv::Mat out;
    if (gray) {
      if (img.channels() == 3) cv::cvtColor(img, out, cv::COLOR_BGR2GRAY);
      else out = img;
    } else {
      out = img;
    }

    char name[64];
    std::snprintf(name, sizeof(name), "%06d.png", saved);
    const std::string path = out_dir + "/" + name;
    const auto t_write0 = std::chrono::steady_clock::now();
    if (!cv::imwrite(path, out)) {
      tools::logger()->error("写 {} 失败（权限 / 磁盘空间 / 扩展名）", path);
      return 1;
    }
    const double write_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t_write0).count();

    ++saved;
    next_due += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
    // 相机是自由跑（实测 93~132 fps），节流点几乎总落在两帧相机画面之间，
    // 晚几毫秒是必然的、不是"跟不上"。只有整整晚掉一拍（> 1 个 period）才值得报警。
    // 之前每帧都打这条 warning，200 帧刷了 60 多行，纯噪音。
    if (next_due < std::chrono::steady_clock::now()) {
      const auto lag = std::chrono::steady_clock::now() - next_due;
      if (lag > std::chrono::duration_cast<std::chrono::steady_clock::duration>(period)) {
        tools::logger()->warn("第 {} 帧：落盘落后 {:.0f} ms（>1 拍），已重新对齐时基",
                              saved, std::chrono::duration<double>(lag).count() * 1000.0);
      }
      next_due = std::chrono::steady_clock::now();
    }
    last_kept = std::chrono::steady_clock::now();

    if (saved % 20 == 0) {
      const double elapsed =
        std::chrono::duration<double>(last_kept - t_start).count();
      tools::logger()->info("已录 {}/{} 帧  实际 {:.1f} 帧/秒  画面 {}x{}（本帧存盘 {:.0f} ms）",
                            saved, want_frames,
                            elapsed > 0 ? saved / elapsed : 0.0,
                            out.cols, out.rows, write_ms);
    }
  }

  const double elapsed = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - t_start).count();
  tools::logger()->info("完成：{} 帧，用时 {:.2f}s（请求 {:.1f} fps，实际 {:.1f} fps），空帧跳过 {} 次",
                        saved, elapsed, fps, saved / std::max(1e-6, elapsed), skipped_empty);
  tools::logger()->info("拷到 Windows 后跑：scripts\\准备素材.bat \"<这个目录>\"");
  return 0;
}
