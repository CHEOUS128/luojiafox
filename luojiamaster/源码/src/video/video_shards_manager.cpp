#include "video_shards_manager.h"

namespace RoboGo::video {

    VideoShardsManager::~VideoShardsManager() {
        reset_stream_state();
        drain_packet_queue();
    }

    void VideoShardsManager::set_frame_callback(const std::function<void(const std::vector<uint8_t> &frame)> &frame_callback) {
        this->frame_callback = frame_callback;
        SPDLOG_INFO("已设置视频完整帧回调");
    }

    void VideoShardsManager::on_receive_shard(const uint8_t *data, size_t size) {
        auto *packet = new uint8_t[size];
        memcpy(packet, data, size);
        // 队列满了绝不能阻塞网络线程，也不能无限增长（会耗死整个进程）。直接丢掉这个分片。
        // 注意：队列元素类型是 pair<uint8_t*, size_t>，try_enqueue 必须把整个 pair 一次传进去。
        if (!packet_queue.try_enqueue(std::make_pair(packet, size))) {
            delete[] packet;
        }
    }

    void VideoShardsManager::handle_once() {
        static std::uint64_t s_dropped_shards = 0;
        // 如果数据包过多导致阻塞，就丢弃旧的数据包
        if (packet_queue.size_approx() >= 4096) {
            std::size_t trimmed_count = 0;
            std::pair<uint8_t *, size_t> stale_packet;
            while (packet_queue.size_approx() > 2048 && packet_queue.try_dequeue(stale_packet)) {
                delete[] stale_packet.first;
                ++trimmed_count;
            }
            // 解析不过来时几乎每个分片都会触发一次，所以限制成每 256 个才写一行。
            s_dropped_shards += trimmed_count;
            if (trimmed_count > 0 && (s_dropped_shards & 0xFF) < trimmed_count) {
                SPDLOG_WARN("UDP 视频分片队列堆积，已丢弃旧分片累计 {} 个", s_dropped_shards);
            }
        }
        // 获取一个数据包
        std::pair<uint8_t *, size_t> packet;
        // 队列空时最多等 200 微秒。原来只等 1 微秒，解码线程空转就把一个核吃满；
        // 有包进来会立刻唤醒，不影响延迟。
        if (!packet_queue.wait_dequeue_timed(packet, std::chrono::microseconds(200))) {
            return;
        }
        auto [data, size] = packet;
        // 解析包头
        if (size <= 8) [[unlikely]] {
            SPDLOG_WARN("收到无效视频分片 bytes={}", size);
            delete[] data;
            return;
        }
        uint16_t frame_id = ntohs(*reinterpret_cast<const uint16_t *>(data));
        uint16_t shard_id = ntohs(*reinterpret_cast<const uint16_t *>(data + 2));
        uint32_t frame_bytes = ntohl(*reinterpret_cast<const uint32_t *>(data + 4));

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
                delete[] data;
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
                        iter->second.second,
                        size);
            delete[] iter->second.first;
            current_frame_shards.collected_frame_bytes -= static_cast<uint32_t>(iter->second.second - 8);
        }
        current_frame_shards.collected_frame_bytes += size - 8;
        current_frame_shards.shards[shard_id] = {data, size};

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
                auto &[shard_buffer, shard_size] = iter->second;
                full_frame.insert(full_frame.end(), shard_buffer + 8, shard_buffer + shard_size);
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
        std::pair<uint8_t *, size_t> packet;
        while (packet_queue.try_dequeue(packet)) {
            delete[] packet.first;
        }
    }

    void VideoShardsManager::reset_frame_shards(FrameShards &frame_shards) {
        for (const auto &item: frame_shards.shards | std::views::values) {
            delete[] item.first;
        }
        frame_shards.is_valid = false;
        frame_shards.frame_id = 0;
        frame_shards.frame_bytes = 0;
        frame_shards.collected_frame_bytes = 0;
        frame_shards.shards.clear();
    }

}// namespace RoboGo::video
