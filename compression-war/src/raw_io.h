// raw_io.h —— 读写模拟器的两种中间产物
//
// 1. 编码器输入：ffmpeg 写出的 rawvideo（gray 或 yuv420p），无文件头
// 2. 编码器输出：ffmpeg 写出的 Annex-B H.264，按 AUD 切回每帧
//
// 为什么用「中间文件」而不是全程管道：
//   压片战争要反复对比「同一个输入、不同编码参数」。把取景+缩放+编码的产物
//   落盘一次，之后跑不同包策略/解码策略都不用重复编码（编码很慢）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cw {

// 一帧原始图像。
struct RawFrame {
    int width = 0;
    int height = 0;
    // gray    : bytes = W*H        （Y 平面，没有色度）
    // yuv420p : bytes = W*H*3/2    （YUV420P 的连续三平面）
    // bgr24   : bytes = W*H*3      （相机原生 BGR，播放器左栏「原封不动」用）
    bool gray = true;
    bool bgr = false;
    std::vector<std::uint8_t> bytes;
};

// 像素格式的规范写法（gray / yuv420p / bgr24）。
// 各种常见别名先归一，再由 frame_bytes_for() 给单帧字节数。
bool normalize_pix_fmt(const std::string & in, std::string & out);
std::size_t frame_bytes_for(const std::string & pix_fmt, int width, int height);

// 读一个 PNG 帧目录（000000.png、000001.png …），**不做任何像素处理**。
// 给播放器左栏用：相机录下来什么就是什么（彩色就彩色、灰度就灰度）。
// 这是最忠实的一条路——不需要先转成 .bgr/.gray 中间文件。
// stb_image 解出 RGB，这里翻成 BGR 存（和 OpenCV/相机一致）。
bool read_png_frames(const std::string & dir,
                     int max_frames,
                     std::vector<RawFrame> & out,
                     std::string & error);

// 把整个 raw 文件读成帧序列。frames<0 表示全读。
// stride 必须是紧排的（ffmpeg rawvideo 默认就是紧排）。
// pix_fmt: gray | yuv420p | bgr24（见 normalize_pix_fmt）
bool read_raw_frames(const std::string & path,
                     int width,
                     int height,
                     const std::string & pix_fmt,
                     int max_frames,
                     std::vector<RawFrame> & out,
                     std::string & error);

// 一帧 H.264 访问单元（Annex-B，以 00 00 01 或 00 00 00 01 开头）
struct AccessUnit {
    std::vector<std::uint8_t> bytes;
    bool keyframe = false;
};

// 按 AUD（NAL type 9）把 Annex-B 码流切开，返回每个访问单元。
// 如果没有 AUD，退化成按「第一个 VCL NAL 且前面已出现过 VCL」切分。
bool split_access_units(const std::string & h264_path,
                        std::vector<AccessUnit> & out,
                        std::string & error);

}  // namespace cw
