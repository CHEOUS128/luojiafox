// packetizer.h —— 发送端：H.264 访问单元 → 300 B CustomByteBlock 包 → 令牌桶出包
//
// 这是 VideoSender::encode_and_pack / make_transport_packet / drain_due
// 的**逐字节复刻**。之所以要复刻而不是共用源码：
//   VideoSender 依赖 autoaim 的整套 io/camera/opencv，搬不过来；
//   而这段逻辑只有 100 行，复刻后可以用「和实车实测对账」来验证它没抄错。
//
// 对账基准（档案/包率85%+灰度版，300 帧真实赛场画面，192x152 GOP10 灰度 59kbps）：
//   均值 371 B/帧、1.777 包/帧、35.5 包/秒、SSIM-Y 0.9853
#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "sim_config.h"

namespace cw {

struct WirePacket {
    std::vector<std::uint8_t> bytes;   // 正好 300 B
    std::uint32_t frame_index = 0;     // 属于哪一帧（仿真用，不上下线）
    std::uint16_t inner_seq = 0;
};

class Packetizer {
public:
    explicit Packetizer(const TransportSpec & spec);

    // 把一帧的 H.264 访问单元切成若干个 300 B 包。
    // 返回空 = 这一帧超过 max_packets_per_frame × max_payload，整帧丢弃（与实车一致）。
    // 传进 pending 的顺序与实车一致：整帧的包连续入队，不会被别的帧插队。
    bool push_frame(const std::vector<std::uint8_t> & access_unit,
                    std::uint32_t frame_index,
                    std::vector<WirePacket> & out);

    std::uint64_t frames_oversize() const { return frames_oversize_; }

private:
    TransportSpec spec_;
    std::uint16_t inner_seq_ = 0;
    std::uint64_t frames_oversize_ = 0;
};

// 令牌桶 + 整帧原子释放，复刻 VideoSender::drain_due 的大帧模式。
class TokenBucket {
public:
    explicit TokenBucket(const TransportSpec & spec);

    // 把一帧的包放进待发队列（整帧一起，不会被拆开）
    void enqueue_frame(std::vector<WirePacket> packets);

    // 推进到 now_ms，放出到点的包。返回按序释放的包。
    // 超出积压上限（max(2, fps) 帧）时丢最老的整帧，计入 backlog_dropped。
    void drain(double now_ms, double fps, std::vector<WirePacket> & out);

    // 结束时把剩余全放出来（不计令牌）
    void flush(std::vector<WirePacket> & out);

    std::uint64_t backlog_dropped() const { return backlog_dropped_; }
    std::uint64_t frames_released() const { return frames_released_; }
    double packets_released() const { return packets_released_; }
    // flush() 强行放出的包数。它们在令牌桶眼里没花令牌，但**确实上了网**，
    // 统计「收/发」时必须算进去，否则会出现「收 > 发」这种荒唐数。
    double packets_flushed() const { return packets_flushed_; }
    double tokens() const { return tokens_; }
    std::size_t pending_frames() const { return pending_frame_sizes_.size(); }

private:
    TransportSpec spec_;
    double tokens_ = 0.0;
    double last_tick_ms_ = -1.0;
    bool has_last_tick_ = false;
    std::deque<WirePacket> pending_;
    std::deque<std::size_t> pending_frame_sizes_;
    std::uint64_t backlog_dropped_ = 0;
    std::uint64_t frames_released_ = 0;
    double packets_released_ = 0.0;
    double packets_flushed_ = 0.0;
};

}  // namespace cw
