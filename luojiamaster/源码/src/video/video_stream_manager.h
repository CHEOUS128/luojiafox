#pragma once

#include "io/video_stream_receiver.h"
#include "video/video_decoder.h"
#include "video/video_shards_manager.h"
#include <pch.h>

namespace RoboGo::video {

    class VideoStreamManager {
    private:
        std::unique_ptr<VideoDecoder> video_decoder;
        VideoShardsManager video_shards_manager;
        std::thread decode_thread;
        bool stream_ready = false;
        std::atomic<bool> is_running = true;
        // 原始码流旁路（队友画面转发用）。为空时完全不做事，原有行为不受影响。
        // 只在分片组装出完整帧时、解码之前调用，拿到的是和官方图传一模一样的 HEVC 字节。
        std::function<void(const std::vector<uint8_t> &frame)> raw_frame_callback;
        // Construct receiver last: its async callback captures all state above.
        std::unique_ptr<io::VideoStreamReceiver> video_stream_receiver;

    public:
        using FrameHandler = std::function<void(const AVFrame *)>;
        using RawFrameHandler = std::function<void(const std::vector<uint8_t> &)>;
        // This constructor starts only the decoder; a recording can supply datagrams directly.
        explicit VideoStreamManager(const FrameHandler &frame_callback, RawFrameHandler raw_frame_handler = {});
        explicit VideoStreamManager(boost::asio::io_context &io_context,
                                    boost::asio::ip::udp::endpoint endpoint,
                                    const std::function<void(const AVFrame *frame)> &frame_callback,
                                    std::function<void(const std::vector<uint8_t> &frame)> raw_frame_callback = {});
        ~VideoStreamManager();

        void receive_datagram(const uint8_t *data, size_t size);
        void restart_recording() { video_shards_manager.request_stream_reset(); }
        void stop();
    };

}// namespace RoboGo::video
