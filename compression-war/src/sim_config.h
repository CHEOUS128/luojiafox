// sim_config.h —— 压片仿真器的全部可调参数
//
// 设计原则：字段名尽量与 autoaim 的 standard3.yaml 对齐，
// 这样实验结论能原样搬回车上，不用翻译。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cw {

// 编码线程的时序表（--encode-ms > 0 时由 encoder.cpp 算出来）
//
// ★ 为什么需要这张表：车端 VideoSender 的编码线程只有一个 pending_frame，
//   注释写着「待编码帧（只保留最新一帧，慢于 fps 时自动丢旧帧）」。
//   编码慢于相机节拍时，还没被取走的旧帧会被**直接覆盖**——那些帧从来没有进过
//   x264。所以不能「全部编完再把包丢掉」：P 帧会引用车端根本不存在的参考帧，
//   后面的解码链就断了。正确做法是从编码输入里就把它们去掉。
//
//   au_camera[j]  = 第 j 个访问单元来自相机第几帧
//   au_done_ms[j] = 那一帧编码完成的时刻（= 车端 encode_and_pack 返回的时刻，
//                   包就是从这一刻开始排队等令牌）
struct EncodeTimeline {
    std::size_t camera_frames = 0;          // 相机一共来了多少帧
    std::vector<std::uint32_t> au_camera;   // 大小 == 访问单元数
    std::vector<double> au_done_ms;         // 大小 == 访问单元数
    std::string input_path;                 // 真正喂给 ffmpeg 的 raw（丢过帧时是过滤后的副本）
};

// 输入素材：编码器"看得见"的那一帧，已经取景+缩放过
struct SourceSpec {
    std::string raw_path;                        // ffmpeg 产出的原始帧
    int width = 192;
    int height = 152;
    std::string pix_fmt = "gray";                // gray | yuv420p（编码器输入）

    // ---- 播放器左侧用的「真原片」----
    // 编码器输入只有 192×152，拿它当"左侧原图"，人眼看不出到底是压缩糊了还是
    // 本来就糊。所以另给一路同样取景、但保留细节的原片（1280×1024）。
    //
    // ★ CH 2026-11-07 定的语义（这是播放器的根本规矩，别改）：
    //   左侧 = 相机原片，**原封不动**——不转灰度、不缩放、不锐化、不做任何处理。
    //   灰度、缩放、H.264 全都是"我们这边后续做的处理"，只能出现在右侧。
    //   所以左栏优先用彩色 bgr24（相机原生），没有彩色原片才退回灰度。
    //
    // ⚠ 它**只影响播放器显示**，不参与编码/封包/统计——仿真链路仍然只吃 raw_path。
    //   这一点必须保持：实车上 VideoSender 拿到的就是相机原图缩到 192×152 之后的样子，
    //   把 1280 塞进编码器会变成一个不存在的链路。
    // 两种给法：
    //   ref_path 指向一个 **PNG 帧目录**（000000.png…）→ 原封不动读进来，最忠实（推荐）
    //   ref_path 指向裸帧文件 + ref_pix_fmt → gray / bgr24 紧排数据
    std::string ref_path;
    std::string ref_pix_fmt = "gray";            // gray | bgr24（相机原生是 BGR）
    bool ref_is_png_dir = false;                 // true = ref_path 是 PNG 目录
    int ref_width = 0;
    int ref_height = 0;

    // 只跑源文件的前 N 帧（-1 = 全跑）。
    // ⚠ 它必须同时作用在「ffmpeg 编码」和「读 raw 帧」两处：
    //   只限制后者会让包数/帧数和码流对不上，只限制前者会白读几百 MB 原始帧。
    int max_frames = -1;
};

// 编码产物：ffmpeg 用目标参数压出来的 H.264
struct StreamSpec {
    std::string h264_path;
    double fps = 20.0;
};

// 传输层：协议上限与令牌桶
struct TransportSpec {
    // ---- 协议硬上限（客户端侧的固定常量，不能改）----
    std::size_t packet_bytes = 300;              // 一个 CustomByteBlock 包 300 B
    std::size_t transport_prefix_bytes = 2;      // 内层长度字段
    std::size_t inner_seq_bytes = 2;             // 内层序号字段

    // ★ 比赛规则给的天花板：RM 2026 通信协议 V2.0.0，0x0310 数据段 300 B、频率上限 50 Hz。
    //   300 B × 50 Hz = 15,000 B/s = 120 kbps 空口字节率（不是净载荷率，净载荷是 296×50）。
    //   ⚠ 仿真里任何档位都必须 ≤ 这两个数，超了就说明"这个配置在比赛里根本发不出去"，
    //     跑出来的包率和画质全是假的。main.cpp 里有硬校验，别绕过。
    static constexpr double protocol_max_send_rate_hz = 50.0;
    static constexpr std::size_t protocol_packet_bytes = 300;
    static constexpr double protocol_ceiling_bps() {
        return protocol_max_send_rate_hz * static_cast<double>(protocol_packet_bytes) * 8.0;
    }

    // ---- 发送端策略 ----
    double send_rate_hz = 40.0;                  // 包/秒（协议上限 50；实车 yaml 配的是 40）
    double budget_ratio = 0.85;                  // 令牌桶只用多少（CH 定的 85%）
    int max_packets_per_frame = 8;               // 一帧最多拆几个包
    std::size_t max_payload_bytes = 296;         // 每包 H.264 净载荷上限

    // ---- 丢包模拟 ----
    double drop_rate = 0.0;                      // 随机丢包概率（0 = 理论档）
    std::uint32_t seed = 20261107u;

    // 派生量
    std::size_t max_payload() const { return packet_bytes - transport_prefix_bytes - inner_seq_bytes; }
};

// 一次仿真运行的汇总
struct RunSummary {
    std::string name;
    // 输入
    std::size_t frames_camera = 0;               // 相机一共来了多少帧（= feed() 放行数，见窗口口径）
    std::size_t frames_encode_dropped = 0;       // ★ 编码线程跟不上 → pending_frame 被新帧覆盖
    std::size_t frames_encoded = 0;
    // 包层
    std::uint64_t packets_emitted = 0;           // 传输窗口内令牌桶放出的（口径 = 实车日志的「发包」）
    std::uint64_t packets_tail = 0;              // 收尾那 1 秒里放出的（真实行为，但在测量窗口外）
    std::uint64_t packets_flushed = 0;           // 跑完仍积压、强行放出的（实车会判积压丢掉）
    std::uint64_t packets_delivered = 0;
    std::uint64_t packets_dropped_budget = 0;    // 令牌不够、整帧未发
    std::uint64_t packets_dropped_network = 0;   // 模拟网络丢包
    std::uint64_t packets_dropped_backlog = 0;   // 积压超 1 秒丢最老帧
    double packets_per_second = 0.0;             // 传输窗口内的稳态包率（物理上封顶 send_rate_hz）
    // 含收尾尾巴的包率（有限帧数跑到结束，尾巴也除进窗口）。档案的 35.5/41.1 大概率是这个口径。
    // ⚠ 它只解释得动约 0.3 包/秒的差；对不上 41.1 的大头在素材（帧大小分布不同 → ceil 取整浪费不同），
    //   不是链路复刻错了。详见 README 第五节。
    double packets_per_second_incl_tail = 0.0;
    // ★ 需求包率 = Σ ceil(帧字节/296) / 时长。档案里的 35.5 / 41.1 和 88.8% / 102.8%
    //   就是这个口径：它数的是"这些帧一共要多少包"，**不是令牌桶放出了多少包**。
    //   所以它能超过 send_rate_hz（102.8% > 100%），放行包率则永远不能——
    //   实车链路里那超过 100% 的部分会以积压形式欠着，欠久了就丢帧。
    double packets_demand_per_second = 0.0;
    double demand_utilisation = 0.0;
    double send_rate_hz = 0.0;                   // 本次用的令牌桶速率（算百分比的基准）
    double budget_utilisation = 0.0;             // 实际包率 / send_rate_hz（满桶=100%）
    // 帧层
    std::uint64_t frames_oversize = 0;           // 超过「单帧字节上限」被整帧丢弃
    std::uint64_t frames_decoded = 0;            // 客户端真正解出来的帧数
    std::uint64_t frames_lost = 0;               // 有包进网、但客户端一帧没解出来
    double decode_fps = 0.0;
    // 延迟（从「这一帧的第一个包发出」到「这一帧最后一个包发出」）
    double frame_completion_ms_median = 0.0;
    double frame_completion_ms_p95 = 0.0;
    // 画质
    double psnr_y_mean = 0.0;
    double ssim_y_mean = 0.0;
    double ssim_y_min = 1.0;
    // 去掉「整体色调偏置」后的 SSIM，只反映压缩损失。
    // 两者差得越大，说明越多的画质损失来自那条 sws 满量程→limited 展开的约定错配，
    // 而不是来自码率不够。详见 metrics.h。
    double ssim_y_tone_free_mean = 0.0;
    // 拟合出来的色调曲线 dec ≈ tone_k * src + tone_c（逐帧拟合后取中位）
    double tone_k_median = 1.0;
    double tone_c_median = 0.0;
    // 包大小分布
    std::size_t frame_bytes_median = 0;
    std::size_t frame_bytes_p95 = 0;
    std::size_t frame_bytes_max = 0;
    double packets_per_frame_mean = 0.0;
};

}  // namespace cw
