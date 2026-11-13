#include "video_shards_manager.h"

namespace RoboGo::video {

    VideoShardsManager::~VideoShardsManager() {
        stop();
        reset_stream_state();
        drain_packet_queue();
    }

    void VideoShardsManager::set_frame_callback(const std::function<void(const std::vector<uint8_t> &frame)> &frame_callback) {
        this->frame_callback = frame_callback;
        SPDLOG_INFO("已设置视频完整帧回调");
    }

    void VideoShardsManager::on_receive_shard(const uint8_t *data, size_t size) {
        if (data == nullptr || size <= 8 || size > 65535) return;
        std::vector<uint8_t> packet(data, data + size);
        std::deque<std::vector<uint8_t>> discarded;
        {
            std::lock_guard lock(packet_mutex);
            if (!accepting) return;
            if (packet_queue.size() >= max_queued_packets || queued_bytes + size > max_queued_bytes) {
                discarded.swap(packet_queue);
                queued_bytes = 0;
                reset_pending = true;
            }
            queued_bytes += size;
            packet_queue.push_back(std::move(packet));
        }
        packet_ready.notify_one();
    }

    void VideoShardsManager::handle_once() {
        std::vector<uint8_t> packet;
        bool needs_reset = false;
        {
            std::unique_lock lock(packet_mutex);
            packet_ready.wait_for(lock, std::chrono::milliseconds(5), [this]() {
                return !accepting || !packet_queue.empty();
            });
            if (!accepting || packet_queue.empty()) return;
            needs_reset = reset_pending;
            reset_pending = false;
            packet = std::move(packet_queue.front());
            queued_bytes -= packet.size();
            packet_queue.pop_front();
        }
        if (needs_reset) {
            reset_stream_state();
            if (reset_callback) reset_callback();
        }
        const auto *data = packet.data();
        const auto size = packet.size();
        // 解析包头
        if (size <= 8) [[unlikely]] {
            SPDLOG_WARN("收到无效视频分片 bytes={}", size);
            return;
        }
        uint16_t frame_id = ntohs(*reinterpret_cast<const uint16_t *>(data));
        uint16_t shard_id = ntohs(*reinterpret_cast<const uint16_t *>(data + 2));
        uint32_t frame_bytes = ntohl(*reinterpret_cast<const uint32_t *>(data + 4));
        if (frame_bytes == 0 || frame_bytes > max_frame_bytes || size - 8 > frame_bytes) {
            SPDLOG_WARN("Invalid UDP video frame size bytes={} shard_bytes={}", frame_bytes, size);
            return;
        }

        // 处理帧 ID 溢出问题
        if (!is_current_frame_id_valid) {
            is_current_frame_id_valid = true;
            current_frame_id = frame_id;
            SPDLOG_INFO("初始化视频帧序列 current_frame_id={}", current_frame_id);
        }
        uint32_t fixed_frame_id = fixed_frame_id_start + frame_id;
        int64_t delta_frame_id = static_cast<int64_t>(fixed_frame_id) - current_frame_id;
        if (delta_frame_id <= -100 && frame_id < cache_frame_size) {
            fixed_frame_id = max_frame_id + 1;
            fixed_frame_id_start = fixed_frame_id - frame_id;
            delta_frame_id = 1;
            SPDLOG_INFO("视频帧ID发生回绕 frame_id={} fixed_frame_id={}", frame_id, fixed_frame_id);
        }
        max_frame_id = std::max(max_frame_id, fixed_frame_id);

        // 如果接收到过去的数据包，就丢掉
        if (delta_frame_id < 0) [[unlikely]] {
            if (delta_frame_id < -static_cast<int64_t>(cache_frame_size)) {
                // 流断连后重连或编码器重置，重置帧序列
                SPDLOG_WARN("视频流序列不连续，重置帧序列 old_current={} new_frame_id={}",
                            current_frame_id, frame_id);
                reset_stream_state();
                is_current_frame_id_valid = true;
                current_frame_id = frame_id;
                fixed_frame_id = frame_id;
                delta_frame_id = 0;
            } else {
                SPDLOG_WARN("丢弃过期视频分片 frame_id={} shard_id={} current_frame_id={}",
                            fixed_frame_id, shard_id, current_frame_id);
                return;
            }
        }

        // 处理丢帧
        if (delta_frame_id >= cache_frame_size) [[unlikely]] {
            SPDLOG_WARN("视频帧缓存被冲刷 delta_frame_id={} cache_frame_size={}", delta_frame_id, cache_frame_size);
            for (size_t i = 0; i < cache_frame_size; ++i) {
                size_t target_index = i + delta_frame_id - cache_frame_size + 1;
                if (target_index < cache_frame_size) {
                    std::swap(cache_frames[i], cache_frames[target_index]);
                } else {
                    reset_frame_shards(cache_frames[i]);
                }
            }
            current_frame_id = fixed_frame_id - cache_frame_size + 1;
            delta_frame_id = cache_frame_size - 1;
        }

        // 把数据包添加到缓存中
        auto &current_frame_shards = cache_frames[delta_frame_id];
        if (!current_frame_shards.is_valid) [[unlikely]] {
            current_frame_shards.is_valid = true;
            current_frame_shards.frame_id = fixed_frame_id;
            current_frame_shards.frame_bytes = frame_bytes;
        } else if (current_frame_shards.frame_bytes != frame_bytes) {
            SPDLOG_WARN("视频帧长度发生变化 frame_id={} old_bytes={} new_bytes={}",
                        fixed_frame_id,
                        current_frame_shards.frame_bytes,
                        frame_bytes);
            current_frame_shards.frame_bytes = frame_bytes;
        }
        if (const auto iter = current_frame_shards.shards.find(shard_id); iter != current_frame_shards.shards.end()) [[unlikely]] {
            SPDLOG_WARN("收到重复视频分片 frame_id={} shard_id={} old_size={} new_size={}",
                        fixed_frame_id,
                        shard_id,
                        iter->second.size(),
                        size);
            current_frame_shards.collected_frame_bytes -= static_cast<uint32_t>(iter->second.size() - 8);
        }
        if (current_frame_shards.collected_frame_bytes + size - 8 > max_frame_bytes) {
            reset_frame_shards(current_frame_shards);
            return;
        }
        current_frame_shards.collected_frame_bytes += size - 8;
        current_frame_shards.shards[shard_id] = std::move(packet);

        // 处理完整帧
        auto &first_frame_shards = cache_frames[0];
        if (first_frame_shards.is_valid && first_frame_shards.frame_bytes == first_frame_shards.collected_frame_bytes) [[unlikely]] {
            // 检测跳帧
            if (last_complete_frame_id.has_value() && last_complete_frame_id.value() + 1 != first_frame_shards.frame_id) [[unlikely]] {
                SPDLOG_ERROR("skip frame：{} -> {}", last_complete_frame_id.value(), first_frame_shards.frame_id);
            }
            last_complete_frame_id = first_frame_shards.frame_id;
            // 拼接
            full_frame.clear();
            full_frame.reserve(first_frame_shards.frame_bytes);
            bool is_success = true;
            for (size_t j = 0; j < first_frame_shards.shards.size(); ++j) {
                const auto &iter = first_frame_shards.shards.find(j);
                if (iter == first_frame_shards.shards.end()) [[unlikely]] {
                    is_success = false;
                    SPDLOG_ERROR("分片ID错误");
                    break;
                }
                const auto &shard = iter->second;
                full_frame.insert(full_frame.end(), shard.begin() + 8, shard.end());
            }
            if (is_success) [[likely]] {
                if (frame_callback) {
                    frame_callback(full_frame);
                } else {
                    SPDLOG_WARN("视频完整帧已生成但未设置回调 frame_id={}", first_frame_shards.frame_id);
                }
            }
            // 在缓存中清理当前帧
            for (size_t j = 0; j < cache_frame_size - 1; ++j) {
                std::swap(cache_frames[j], cache_frames[j + 1]);
            }
            reset_frame_shards(cache_frames[cache_frame_size - 1]);
            current_frame_id += 1;
        }
    }

    void VideoShardsManager::reset_stream_state() {
        for (auto &frame_shards: cache_frames) {
            reset_frame_shards(frame_shards);
        }
        fixed_frame_id_start = 0;
        max_frame_id = 0;
        is_current_frame_id_valid = false;
        current_frame_id = 0;
        last_complete_frame_id.reset();
        full_frame.clear();
    }

    void VideoShardsManager::drain_packet_queue() {
        std::deque<std::vector<uint8_t>> discarded;
        {
            std::lock_guard lock(packet_mutex);
            discarded.swap(packet_queue);
            queued_bytes = 0;
        }
    }

    void VideoShardsManager::stop() {
        {
            std::lock_guard lock(packet_mutex);
            accepting = false;
        }
        packet_ready.notify_all();
        drain_packet_queue();
    }

    void VideoShardsManager::reset_stream() {
        drain_packet_queue();
        reset_stream_state();
        std::lock_guard lock(packet_mutex);
        reset_pending = false;
    }

    void VideoShardsManager::reset_frame_shards(FrameShards &frame_shards) {
        frame_shards.is_valid = false;
        frame_shards.frame_id = 0;
        frame_shards.frame_bytes = 0;
        frame_shards.collected_frame_bytes = 0;
        frame_shards.shards.clear();
    }

}// namespace RoboGo::video
