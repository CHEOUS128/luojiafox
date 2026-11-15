#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace RoboGo::app::performance {
using TraceClock = std::chrono::steady_clock;

struct DataArrival {
    bool known = false;
    std::uint64_t loop = 0, first_packet = 0, last_packet = 0;
    TraceClock::time_point first{}, complete{};
};

struct FrameTrace {
    std::int64_t token = -1;
    DataArrival arrival;
    TraceClock::time_point decode_submit{}, decoded{}, converted{};
    double conversion_us = 0, copy_us = 0, conversion_total_us = 0;
    std::uint64_t copied_bytes = 0;
    bool conversion_allocation = false;
};

// All timestamps are local actual ingress times, never recorded/source times.
// A complete UDP byte block is accepted only after checking all its real bytes.
class UdpArrivalRegistry {
    struct Piece { std::span<const std::uint8_t> bytes; TraceClock::time_point arrived; std::uint64_t packet; };
    struct Block {
        std::uint32_t expected = 0;
        std::uint64_t first_record = 0;
        bool ambiguous = false;
        std::map<std::uint16_t, Piece> pieces;
    };
    using Key = std::pair<std::uint64_t, std::uint16_t>;
    std::mutex mutex_;
    std::map<Key, Block> blocks_;
    std::deque<Key> insertion_;
    static constexpr std::size_t capacity = 4096;
public:
    void Add(std::uint64_t loop, std::uint64_t packet, std::span<const std::uint8_t> bytes,
             TraceClock::time_point arrived) {
        if (bytes.size() <= 8) return;
        const auto be16 = [&](std::size_t i) { return std::uint16_t((std::uint16_t(bytes[i]) << 8) | bytes[i+1]); };
        const auto frame = be16(0), shard = be16(2);
        const auto size = (std::uint32_t(bytes[4]) << 24) | (std::uint32_t(bytes[5]) << 16)
                        | (std::uint32_t(bytes[6]) << 8) | bytes[7];
        if (size == 0 || size > 8U * 1024U * 1024U || bytes.size() - 8 > size) return;
        const Key key{loop, frame};
        std::lock_guard lock(mutex_);
        auto [found, inserted] = blocks_.try_emplace(key);
        if (inserted) {
            insertion_.push_back(key);
            if (blocks_.size() > capacity) { blocks_.erase(insertion_.front()); insertion_.pop_front(); }
        }
        auto &block = found->second;
        if (!block.first_record) block.first_record = packet;
        if (packet > block.first_record && packet - block.first_record > 4096) block.ambiguous = true;
        if (block.expected && block.expected != size) block.ambiguous = true;
        block.expected = size;
        const auto payload = bytes.subspan(8);
        if (auto duplicate = block.pieces.find(shard); duplicate != block.pieces.end()) {
            const auto previous = duplicate->second.bytes;
            if (previous.size() != payload.size() || !std::equal(previous.begin(), previous.end(), payload.begin()))
                block.ambiguous = true;
            return; // Identical repeated bytes do not make data become available later.
        }
        if (block.pieces.size() >= 4096) { block.ambiguous = true; return; }
        block.pieces.emplace(shard, Piece{payload, arrived, packet});
    }

    DataArrival Resolve(std::uint64_t loop, std::uint16_t frame, std::span<const std::uint8_t> assembled) {
        std::lock_guard lock(mutex_);
        DataArrival result;
        const auto found = blocks_.find({loop, frame});
        if (found == blocks_.end()) return result;
        const auto &block = found->second;
        if (block.ambiguous || assembled.size() != block.expected || block.pieces.empty()) return result;
        std::size_t offset = 0, index = 0;
        result.first = TraceClock::time_point::max();
        result.first_packet = std::numeric_limits<std::uint64_t>::max();
        for (const auto &[shard, piece] : block.pieces) {
            if (shard != index++ || offset + piece.bytes.size() > assembled.size()
                || !std::equal(piece.bytes.begin(), piece.bytes.end(), assembled.begin() + offset)) return {};
            offset += piece.bytes.size();
            result.first = std::min(result.first, piece.arrived);
            result.complete = std::max(result.complete, piece.arrived);
            result.first_packet = std::min(result.first_packet, piece.packet);
            result.last_packet = std::max(result.last_packet, piece.packet);
        }
        if (offset != assembled.size()) return {};
        result.known = true; result.loop = loop;
        return result;
    }
};

// Causal byte provenance. Parser offsets and every AU byte must agree with the
// actual supplied byte stream; output order or current input token never maps it.
class CausalByteOrigins {
    struct Segment { std::uint64_t begin, end; DataArrival arrival; };
    std::vector<std::uint8_t> bytes_;
    std::deque<Segment> segments_;
    std::uint64_t base_ = 0, end_ = 0;
    std::size_t limit_;
    void Prune(std::uint64_t before) {
        before = std::clamp(before, base_, end_);
        const auto count = static_cast<std::size_t>(before - base_);
        bytes_.erase(bytes_.begin(), bytes_.begin() + static_cast<std::ptrdiff_t>(count));
        base_ = before;
        while (!segments_.empty() && segments_.front().end <= before) segments_.pop_front();
    }
public:
    explicit CausalByteOrigins(std::size_t limit = 16U * 1024U * 1024U) : limit_(limit) {}
    void Reset() { bytes_.clear(); segments_.clear(); base_ = end_ = 0; }
    void Append(std::span<const std::uint8_t> bytes, DataArrival arrival) {
        if (bytes.size() > limit_) { const auto after = end_ + bytes.size(); Reset(); end_ = base_ = after; return; }
        if (bytes_.size() + bytes.size() > limit_) Prune(end_ - (limit_ - bytes.size()));
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
        segments_.push_back({end_, end_ + bytes.size(), arrival});
        end_ += bytes.size();
    }
    DataArrival Resolve(std::span<const std::uint8_t> au, std::int64_t signed_offset) {
        if (signed_offset < 0 || au.empty()) return {};
        const auto offset = static_cast<std::uint64_t>(signed_offset);
        if (offset < base_ || offset > end_ || au.size() > end_ - offset) return {};
        if (!std::equal(au.begin(), au.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(offset - base_))) return {};
        const auto finish = offset + au.size();
        DataArrival result;
        result.known = true; result.first = TraceClock::time_point::max();
        result.first_packet = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t covered = offset;
        std::optional<std::uint64_t> observed_loop;
        for (const auto &segment : segments_) {
            if (segment.end <= offset || segment.begin >= finish) continue;
            if (segment.begin > covered || !segment.arrival.known) return {};
            if (observed_loop && *observed_loop != segment.arrival.loop) return {};
            observed_loop = segment.arrival.loop;
            result.loop = segment.arrival.loop;
            covered = std::min(finish, segment.end);
            result.first = std::min(result.first, segment.arrival.first);
            result.complete = std::max(result.complete, segment.arrival.complete);
            result.first_packet = std::min(result.first_packet, segment.arrival.first_packet);
            result.last_packet = std::max(result.last_packet, segment.arrival.last_packet);
        }
        if (covered != finish) return {};
        Prune(finish);
        return result;
    }
    std::size_t BufferedBytes() const { return bytes_.size(); }
};
} // namespace RoboGo::app::performance
