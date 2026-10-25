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

    // 纯观测计数（原子，供主线程每秒打印一次探针）：
    //   packets_received —— 进入解码器的 MQTT 包数
    //   packets_dequeued —— 真正被解码线程取走的包数（和上面差得多说明解码忙不过来）
    //   frames_output    —— 解码成功产出的帧数
    //   bytes_received   —— 累计字节数
    std::uint64_t packets_received() const {
        return packets_received_.load(std::memory_order_relaxed);
    }
    std::uint64_t packets_dequeued() const {
        return packets_dequeued_.load(std::memory_order_relaxed);
    }
    std::uint64_t frames_output() const {
        return frames_output_.load(std::memory_order_relaxed);
    }
    std::uint64_t bytes_received() const {
        return bytes_received_.load(std::memory_order_relaxed);
    }

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
    std::atomic<std::uint64_t> packets_received_{0};
    std::atomic<std::uint64_t> packets_dequeued_{0};
    std::atomic<std::uint64_t> frames_output_{0};
    std::atomic<std::uint64_t> bytes_received_{0};
};

}// namespace RoboGo::video
