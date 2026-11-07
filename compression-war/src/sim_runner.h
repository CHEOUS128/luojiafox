// sim_runner.h —— 仿真主循环
//
// 时钟模型（这是整个平台最需要想清楚的一处）：
//
//   实车上，相机每 1/fps 秒来一帧，VideoSender 把它放进 pending_frame，
//   编码线程取走后编 encode_ms 毫秒，编完才封包入队；主循环每转一圈调一次
//   drain_due()，令牌桶按真实流逝时间积令牌。
//
//   仿真里不能真的 sleep（跑 100 秒素材要等 100 秒），所以用**虚拟时钟**：
//   每 1 ms 推进一次，相机帧按 1/fps 的节拍放行，编码线程按 encode_ms 占用时间，
//   令牌桶按虚拟时间积令牌。这样丢帧、积压、整帧释放的行为都和实车一致，但瞬时跑完。
//
//   ★ 2026-11-07 起「编码」不再是事后加到延迟上的一个常数，而是**真的占用时间**：
//     编码慢于 1/fps 时，还没被编码线程取走的 pending_frame 会被下一帧覆盖，
//     那些帧根本不会进 x264（车端就是这样，所以 P 帧不会去参考它们）。
//     丢帧数 = 相机帧数 − 访问单元数，summary 里单列 frames_encode_dropped。
//
// ★ 2026-11-07 CH 定的展示规矩（改动这里之前先读一遍）：
//   左边 = 相机原片，原封不动（彩色、未缩放、未转灰度）；
//   右边 = 这一帧处理完以后客户端才收到的画面。
//   「处理」= 缩放 + 转灰度 + 编码（encode_ms，实车在独立线程里跑）+
//             拆包排队 + 令牌桶按 send_rate_hz 放行 + 最后一包到齐 + 客户端解码呈现。
//   所以右边必然比左边晚，播放器会把这段时差实时显示出来。
#pragma once

#include <string>

#include "sim_config.h"

namespace cw {

struct SimResult {
    RunSummary summary;
    std::string per_frame_csv;   // 逐帧一行，方便贴进表格/画图
};

// 完整运行一次仿真。
//   out_png_dir 非空时，把「编码器输入」与「客户端解出」的对照图按帧抽样存成 PNG
//   encode_ms      每帧编码耗时（只用于播放器标注；真正的时序来自 timeline）
//   wire_extra_ms  固定链路/解码开销（目前没有实测值，默认 0）
//   timeline       编码线程时序（encoder.cpp 的 simulate_encode_timeline）。
//                  空表 → 退回「每帧都编、编完=放行」（--h264 直接喂码流 / sweep）。
bool run_simulation(const std::string & raw_path,
                    const std::string & h264_path,
                    const SourceSpec & source,
                    const StreamSpec & stream,
                    const TransportSpec & transport,
                    bool verbose,
                    const std::string & out_png_dir,
                    SimResult & result,
                    std::string & error,
                    const std::string & viewer_dir = "",   // 非空则导出左右对比播放器
                    double encode_ms = 0.0,
                    double wire_extra_ms = 0.0,
                    const EncodeTimeline & timeline = EncodeTimeline{});

// 只跑包化/令牌桶/解码，不写任何文件 —— 给 --sweep 扫参用
bool run_simulation_quiet(const std::string & raw_path,
                          const std::string & h264_path,
                          const SourceSpec & source,
                          const StreamSpec & stream,
                          const TransportSpec & transport,
                          RunSummary & summary,
                          std::string & error);

}  // namespace cw
