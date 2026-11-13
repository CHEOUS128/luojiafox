#include "custom_byte_block_video_decoder_worker.h"

namespace RoboGo::video {

CustomByteBlockVideoDecoderWorker::CustomByteBlockVideoDecoderWorker(FrameHandler frame_handler,
                                                                     LogHandler log_handler,
                                                                     CustomByteBlockVideoDecoderConfig config)
    : config_(std::move(config)),
      frame_handler_(std::move(frame_handler)),
      log_handler_(std::move(log_handler)) {
    // Launch only after every member (including counters/error text) exists.
    worker_thread_ = std::thread([this]() {
          try { run(); } catch (...) {}
          stop_requested_.store(true, std::memory_order_release);
      });
}

CustomByteBlockVideoDecoderWorker::~CustomByteBlockVideoDecoderWorker() {
    stop();
}

void CustomByteBlockVideoDecoderWorker::enqueue_packet_bytes(std::span<const std::uint8_t> wire_bytes) {
    if (wire_bytes.empty() || wire_bytes.size() > 300 || stop_requested_.load(std::memory_order_acquire)) {
        return;
    }

    std::vector<std::uint8_t> packet_bytes(wire_bytes.begin(), wire_bytes.end());
    packets_received_.fetch_add(1, std::memory_order_relaxed);
    bytes_received_.fetch_add(packet_bytes.size(), std::memory_order_relaxed);
    {
        std::lock_guard lock(queue_mutex_);
        if (stop_requested_.load(std::memory_order_acquire)) return;
        if (packet_queue_.size() >= kMaxQueuedPackets) {
            packet_queue_.pop_front();
            packets_dropped_.fetch_add(1, std::memory_order_relaxed);
            reset_requested_ = true;
        }
        packet_queue_.push_back(std::move(packet_bytes));
    }
    queue_cv_.notify_one();
}

void CustomByteBlockVideoDecoderWorker::stop() {
    stop_requested_.store(true, std::memory_order_release);
    std::deque<std::vector<std::uint8_t>> discarded;
    {
        std::lock_guard lock(queue_mutex_);
        discarded.swap(packet_queue_);
    }
    queue_cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void CustomByteBlockVideoDecoderWorker::run() {
    SPDLOG_INFO("CustomByteBlock 解码线程已启动");

    try {
        CustomByteBlockVideoDecoder decoder(config_);

        while (!stop_requested_.load(std::memory_order_acquire)) {
            std::vector<std::uint8_t> packet_bytes;
            bool needs_reset = false;
            {
                std::unique_lock lock(queue_mutex_);
                queue_cv_.wait(lock, [this]() {
                    return !packet_queue_.empty() || stop_requested_.load(std::memory_order_acquire);
                });
                if (stop_requested_.load(std::memory_order_acquire)) break;
                needs_reset = reset_requested_;
                reset_requested_ = false;
                packet_bytes = std::move(packet_queue_.front());
                packet_queue_.pop_front();
            }
            packets_dequeued_.fetch_add(1, std::memory_order_relaxed);

            try {
                if (needs_reset) decoder.reset_stream();
                auto decoded_frames = decoder.push_packet_bytes(packet_bytes);
                for (auto &decoded_frame: decoded_frames) {
                    if (stop_requested_.load(std::memory_order_acquire)) break;
                    frames_output_.fetch_add(1, std::memory_order_relaxed);
                    if (frame_handler_) {
                        frame_handler_(std::move(decoded_frame));
                    }
                    if (!stream_started_logged_) {
                        emit_log(0, "CustomByteBlock 视频流已开始");
                        stream_started_logged_ = true;
                    }
                }
            } catch (const std::exception &error) {
                handle_decode_error(error.what());
            }
        }

        // GUI shutdown discards pending input; flushing is for offline export,
        // not for generating extra images after the window has stopped.
    } catch (const std::exception &error) {
        handle_decode_error(error.what());
    } catch (...) {
        try { emit_log(2, "CustomByteBlock 解码线程出现未知异常，已停止"); } catch (...) {}
    }

    SPDLOG_INFO("CustomByteBlock 解码线程已退出");
}

void CustomByteBlockVideoDecoderWorker::emit_log(const int type, const std::string &text) const {
    if (log_handler_) {
        try { log_handler_(type, text); } catch (...) {}
    }
}

void CustomByteBlockVideoDecoderWorker::handle_decode_error(const std::string &message) {
    SPDLOG_ERROR("CustomByteBlock 视频解码失败 message={}", message);

    const auto now = std::chrono::steady_clock::now();
    if (last_error_text_ != message || now - last_error_time_ >= std::chrono::seconds(1)) {
        last_error_text_ = message;
        last_error_time_ = now;
        emit_log(2, "CustomByteBlock 视频解码失败: " + message);
    }
}

}// namespace RoboGo::video
