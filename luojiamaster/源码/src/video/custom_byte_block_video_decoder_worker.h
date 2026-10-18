#pragma once

#include <pch.h>

#include "video/custom_byte_block_video_decoder.h"

namespace RoboGo::video {

class CustomByteBlockVideoDecoderWorker {
public:
    using FrameHandler = std::function<void(CustomByteBlockDecodedFrame &&frame)>;
    using LogHandler = std::function<void(int type, const std::string &text)>;

    explicit CustomByteBlockVideoDecoderWorker(FrameHandler frame_handler,
                                               LogHandler log_handler,
                                               CustomByteBlockVideoDecoderConfig config = {});
    ~CustomByteBlockVideoDecoderWorker();

    CustomByteBlockVideoDecoderWorker(const CustomByteBlockVideoDecoderWorker &) = delete;
    CustomByteBlockVideoDecoderWorker &operator=(const CustomByteBlockVideoDecoderWorker &) = delete;
    CustomByteBlockVideoDecoderWorker(CustomByteBlockVideoDecoderWorker &&) = delete;
    CustomByteBlockVideoDecoderWorker &operator=(CustomByteBlockVideoDecoderWorker &&) = delete;

    void enqueue_packet_bytes(std::span<const std::uint8_t> wire_bytes);
    void stop();

private:
    void run();
    void emit_log(int type, const std::string &text) const;
    void handle_decode_error(const std::string &message);

    CustomByteBlockVideoDecoderConfig config_;
    moodycamel::BlockingReaderWriterQueue<std::vector<std::uint8_t>> packet_queue_{256};
    FrameHandler frame_handler_;
    LogHandler log_handler_;
    std::atomic<bool> stop_requested_{false};
    std::thread worker_thread_;
    bool stream_started_logged_ = false;
    std::string last_error_text_;
    std::chrono::steady_clock::time_point last_error_time_ =
            std::chrono::steady_clock::now() - std::chrono::seconds(10);
};

}// namespace RoboGo::video
