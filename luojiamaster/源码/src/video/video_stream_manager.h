#pragma once

#include "io/video_stream_receiver.h"
#include "video/video_decoder.h"
#include "video/video_shards_manager.h"
#include <pch.h>

namespace RoboGo::video {

    class VideoStreamManager {
    private:
        io::VideoStreamReceiver video_stream_receiver;
        VideoDecoder video_decoder;
        VideoShardsManager video_shards_manager;
        std::thread decode_thread;
        bool stream_ready = false;
        std::atomic<bool> is_running = true;

    public:
        explicit VideoStreamManager(boost::asio::io_context &io_context,
                                    boost::asio::ip::udp::endpoint endpoint,
                                    const std::function<void(const AVFrame *frame)> &frame_callback);

        void stop();
    };

}// namespace RoboGo::video
