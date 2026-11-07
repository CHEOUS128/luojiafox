#include "packetizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace cw {

namespace {

void write_u16_be(std::uint8_t * p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<std::uint8_t>(v & 0xFF);
}

}  // namespace

Packetizer::Packetizer(const TransportSpec & spec) : spec_(spec) {}

bool Packetizer::push_frame(const std::vector<std::uint8_t> & access_unit,
                            std::uint32_t frame_index,
                            std::vector<WirePacket> & out) {
    out.clear();
    const std::size_t sz = access_unit.size();
    if (sz == 0) return false;

    const std::size_t per = std::min(spec_.max_payload_bytes, spec_.max_payload());
    const std::size_t max_pkts =
      static_cast<std::size_t>(std::clamp(spec_.max_packets_per_frame, 1, 16));
    if (sz > per * max_pkts) {
        ++frames_oversize_;
        return false;   // 与实车一致：超限整帧丢，不拆成更多包
    }

    std::size_t off = 0;
    while (off < sz) {
        const std::size_t n = std::min(per, sz - off);
        WirePacket packet;
        packet.bytes.assign(spec_.packet_bytes, 0);
        packet.frame_index = frame_index;

        // 固定头：[2B 内层长度 BE][2B 内层序号 BE][H.264 ...]，剩余补 0
        const std::uint16_t inner_len = static_cast<std::uint16_t>(spec_.inner_seq_bytes + n);
        write_u16_be(packet.bytes.data(), inner_len);
        write_u16_be(packet.bytes.data() + spec_.transport_prefix_bytes, inner_seq_);
        std::memcpy(packet.bytes.data() + spec_.transport_prefix_bytes + spec_.inner_seq_bytes,
                    access_unit.data() + off,
                    n);
        packet.inner_seq = inner_seq_++;
        out.push_back(std::move(packet));
        off += n;
    }
    return true;
}

TokenBucket::TokenBucket(const TransportSpec & spec) : spec_(spec) {}

void TokenBucket::enqueue_frame(std::vector<WirePacket> packets) {
    if (packets.empty()) return;
    pending_frame_sizes_.push_back(packets.size());
    for (auto & p : packets) pending_.push_back(std::move(p));
}

void TokenBucket::drain(double now_ms, double fps, std::vector<WirePacket> & out) {
    if (!has_last_tick_) {
        last_tick_ms_ = now_ms;
        has_last_tick_ = true;
    }
    const double elapsed_s = std::max(0.0, (now_ms - last_tick_ms_) / 1000.0);
    last_tick_ms_ = now_ms;

    const double cap = static_cast<double>(std::clamp(spec_.max_packets_per_frame, 1, 16));
    // ★ 这里**不能**乘 budget_ratio。实车 drain_due 是按满 send_rate_hz 积令牌，
    //   那 85% 的预算是在「编码码率」那一侧实现的（见 encoder.cpp 的 token_bps 公式）。
    //   如果这里再乘 0.85，就等于把预算扣两遍，仿真会比实车更容易积压丢帧。
    tokens_ = std::min(cap, tokens_ + elapsed_s * spec_.send_rate_hz);

    // 整帧原子释放：令牌够这一帧的全部包才一口气全放
    while (!pending_frame_sizes_.empty()) {
        const std::size_t n = pending_frame_sizes_.front();
        if (tokens_ < static_cast<double>(n)) break;
        if (pending_.size() < n) break;
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(std::move(pending_.front()));
            pending_.pop_front();
        }
        tokens_ -= static_cast<double>(n);
        packets_released_ += static_cast<double>(n);
        ++frames_released_;
        pending_frame_sizes_.pop_front();
    }

    // 积压兜底：超过 max(2, fps) 帧就丢最老的整帧（保持延迟不膨胀）
    const std::size_t backlog_limit =
      static_cast<std::size_t>(std::max(2.0, std::floor(fps)));
    while (pending_frame_sizes_.size() > backlog_limit) {
        const std::size_t n = pending_frame_sizes_.front();
        for (std::size_t i = 0; i < n && !pending_.empty(); ++i) {
            pending_.pop_front();
            ++backlog_dropped_;
        }
        pending_frame_sizes_.pop_front();
    }
}

void TokenBucket::flush(std::vector<WirePacket> & out) {
    while (!pending_.empty()) {
        out.push_back(std::move(pending_.front()));
        pending_.pop_front();
        packets_flushed_ += 1.0;
    }
    pending_frame_sizes_.clear();
}

}  // namespace cw
