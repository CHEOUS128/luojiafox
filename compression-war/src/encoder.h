// encoder.h —— 用外部 ffmpeg 进程把 YUV 原始帧压成 H.264
//
// 为什么不用 AVCodec：仓库里 luojiamaster\源码\3rdparty\ffmpeg 的静态库是**只解不编**的。
// 已两次独立验证（字符串扫描 + 运行时 avcodec_iterate 探针，encoder 数 = 0）。
//
// 好处：编码器成了外部程序，换 x265 / openh264 / VP9 / AV1 只改一行参数，
//       不用动本工程的编译。坏处：必须依赖 ffmpeg.exe 存在（脚本里会检查）。
#pragma once

#include <string>
#include <vector>

#include "sim_config.h"

namespace cw {

// 从 TransportSpec + 目标分辨率 + GOP 反推出实车 VideoSender 会用的那些编码参数。
// 这一步**必须**和 video_sender.cpp 的推导公式一字不差，否则仿真和实车对不上账。
struct EncoderParams {
    double fps = 20.0;
    int gop = 10;
    bool gray = true;
    bool large_frame = true;
    bool single_packet = false;
    int bitrate_bps = 59600;       // 由 token_bps 公式算出
    int vbv_bits = 18944;          // max_frame_packets × max_frame_bytes × 8
    int slices = 4;                // 仅 large_frame 有
    std::string x264_preset = "slow";
    std::string x264_params;       // 完整 x264-params 串
    // = false：现状（实车 VideoSender 就是这样）——sws 输出满量程，但码流不写 range 标记，
    //           客户端解码按 limited range 展开，画面整体压暗、<16 被削成纯黑。
    // = true ：在 SPS 的 VUI 里写 full range。同样的像素，客户端按满量程解释，亮度不失真。
    //           代价：多几个字节 VUI，码率几乎不变。
    bool full_range = false;
    // ★ 0 = 交给 x264 自动（**实车 VideoSender 就是 ctx->thread_count = 0**）。
    //   代价：帧级线程并行 + VBV 会把「哪一帧花多少比特」弄成非确定性的。
    //   实测同一输入同一参数连跑 6 次：包数 349~356（±1%），md5 每次都不同。
    //   所以要对账/复现实验必须给 --threads 1（实测 5 次 md5 完全一致）。
    //   不改成默认值：默认行为必须和实车一字不差。
    int threads = 0;
};

// 复刻 VideoSender 构造函数里的参数推导
EncoderParams derive_encoder_params(const TransportSpec & transport,
                                    const StreamSpec & stream,
                                    bool gray,
                                    bool large_frame,
                                    bool single_packet,
                                    int bitrate_override_bps = 0,
                                    bool full_range = false);

// 生成 ffmpeg 命令行（不含 ffmpeg 自身路径）。
// raw_path 单独传：编码线程丢帧时真正喂给 ffmpeg 的是过滤后的副本，
// 而不是 source.raw_path（source 里还带着原来的 max_frames）。
std::vector<std::string> build_ffmpeg_encode_args(const std::string & raw_path,
                                                  const std::string & h264_path,
                                                  const SourceSpec & source,
                                                  const EncoderParams & params);

// 调用外部 ffmpeg 编码。raw 文件需为纯帧数据（gray: W*H，yuv420p: W*H*3/2）。
bool run_ffmpeg_encode(const std::string & ffmpeg_exe,
                       const std::string & raw_path,
                       const std::string & h264_path,
                       const SourceSpec & source,
                       const EncoderParams & params,
                       bool verbose,
                       std::string & error);

// ---- 编码线程时序（复刻 VideoSender::feed / worker_loop 的 pending_frame 语义）----
//
// 相机按 1/fps 的固定节拍放行一帧（video_sender.cpp:390-401 的累加节拍），
// 编码线程空闲时立刻取走 pending_frame，然后 encode_ms 毫秒不接新帧；
// 这期间到来的帧会覆盖还没被取走的那一帧 —— 被覆盖的帧**永远不属于编码输入**。
//
// 纯计算，不碰文件：可以单独对账（见 --timeline-only）。
EncodeTimeline simulate_encode_timeline(std::size_t camera_frames, double fps, double encode_ms);

// 输入侧丢掉编码线程没来得及取的帧，写出一份**只含存活帧**的 raw 副本。
// 存活帧数 == 访问单元数时返回 source.raw_path，不复制任何东西。
// 注意：不能只限制 ffmpeg 编前 N 帧 —— 存活帧在整段里是不均匀分布的。
bool filter_encoder_input(const std::string & raw_path,
                          int width, int height, const std::string & pix_fmt,
                          const std::string & dst_path,
                          const std::vector<std::uint32_t> & camera_indices,
                          std::size_t camera_frames,
                          std::size_t & frames_written,
                          std::string & error);

}  // namespace cw
