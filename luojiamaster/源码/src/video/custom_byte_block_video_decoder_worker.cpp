#include "custom_byte_block_video_decoder_worker.h"

namespace RoboGo::video {

CustomByteBlockVideoDecoderWorker::CustomByteBlockVideoDecoderWorker(FrameHandler frame_handler,
                                                                     LogHandler log_handler,
                                                                     CustomByteBlockVideoDecoderConfig config)
    : config_(std::move(config)),
      frame_handler_(std::move(frame_handler)),
      log_handler_(std::move(log_handler)),
      worker_thread_([this]() {
          run();
      }) {
}

CustomByteBlockVideoDecoderWorker::~CustomByteBlockVideoDecoderWorker() {
    stop();
}

void CustomByteBlockVideoDecoderWorker::enqueue_packet_bytes(std::span<const std::uint8_t> wire_bytes) {
    if (wire_bytes.empty() || stop_requested_.load(std::memory_order_acquire)) {
        return;
    }

    std::vector<std::uint8_t> packet_bytes(wire_bytes.begin(), wire_bytes.end());
    if (!packet_queue_.enqueue(std::move(packet_bytes))) {
        SPDLOG_ERROR("CustomByteBlock 解码队列入队失败 bytes={}", wire_bytes.size());
        emit_log(2, "CustomByteBlock 解码队列入队失败");
    }
}

void CustomByteBlockVideoDecoderWorker::stop() {
    stop_requested_.store(true, std::memory_order_release);
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void CustomByteBlockVideoDecoderWorker::run() {
    SPDLOG_INFO("CustomByteBlock 解码线程已启动");

    try {
        CustomByteBlockVideoDecoder decoder(config_);

        while (true) {
            std::vector<std::uint8_t> packet_bytes;
            if (!packet_queue_.wait_dequeue_timed(packet_bytes, std::chrono::milliseconds(10))) {
                if (stop_requested_.load(std::memory_order_acquire)) {
                    break;
                }
                continue;
            }

            try {
                auto decoded_frames = decoder.push_packet_bytes(packet_bytes);
                for (auto &decoded_frame: decoded_frames) {
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

        try {
            auto flushed_frames = decoder.flush();
            for (auto &decoded_frame: flushed_frames) {
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
    } catch (const std::exception &error) {
        handle_decode_error(error.what());
    }

    SPDLOG_INFO("CustomByteBlock 解码线程已退出");
}

void CustomByteBlockVideoDecoderWorker::emit_log(const int type, const std::string &text) const {
    if (log_handler_) {
        log_handler_(type, text);
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
