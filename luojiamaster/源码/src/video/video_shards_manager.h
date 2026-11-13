#pragma once

#include <pch.h>

namespace RoboGo::video {

    struct FrameShards {
        bool is_valid = false;
        uint32_t frame_id = 0;
        uint32_t frame_bytes = 0;
        uint32_t collected_frame_bytes = 0;
        std::map<uint16_t, std::vector<uint8_t>> shards;
    };

    class VideoShardsManager {
    private:
        // 缓存数据包相关
        std::mutex packet_mutex;
        std::condition_variable packet_ready;
        std::deque<std::vector<uint8_t>> packet_queue;
        std::size_t queued_bytes = 0;
        bool accepting = true;
        bool reset_pending = false;
        std::function<void()> reset_callback;
        static constexpr std::size_t max_queued_packets = 512;
        static constexpr std::size_t max_queued_bytes = 4U * 1024U * 1024U;
        static constexpr std::uint32_t max_frame_bytes = 8U * 1024U * 1024U;
        // 帧 ID 相关
        uint32_t fixed_frame_id_start = 0;
        uint32_t max_frame_id = 0;
        bool is_current_frame_id_valid = false;
        uint32_t current_frame_id = 0;
        std::optional<uint32_t> last_complete_frame_id;
        // 缓存帧片段相关
        static constexpr int cache_frame_size = 20;
        std::array<FrameShards, cache_frame_size> cache_frames;
        // 完整帧相关
        std::vector<uint8_t> full_frame;
        std::function<void(const std::vector<uint8_t> &frame)> frame_callback;

    public:
        ~VideoShardsManager();

        void set_frame_callback(const std::function<void(const std::vector<uint8_t> &frame)> &frame_callback);

        void on_receive_shard(const uint8_t *data, size_t size);

        void handle_once();
        void stop();
        // Consumer-thread reconnect/replay reset; does not restart a stopped source.
        void reset_stream();
        void set_reset_callback(std::function<void()> callback) { reset_callback = std::move(callback); }

    private:
        void reset_stream_state();

        void drain_packet_queue();

        static void reset_frame_shards(FrameShards &frame_shards);
    };

}// namespace RoboGo::video
