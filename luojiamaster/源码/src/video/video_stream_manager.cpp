#include "video_stream_manager.h"

namespace RoboGo::video {

    VideoStreamManager::VideoStreamManager(boost::asio::io_context &io_context,
                                           boost::asio::ip::udp::endpoint endpoint,
                                           const std::function<void(const AVFrame *frame)> &frame_callback,
                                           std::function<void(const std::vector<uint8_t> &frame)> raw_frame_handler)
        : raw_frame_callback(std::move(raw_frame_handler)),
          video_stream_receiver(io_context, endpoint, [this](const uint8_t *data, size_t size) {
              // 接收到数据包之后送入分片管理
              if (is_running.load(std::memory_order_acquire)) video_shards_manager.on_receive_shard(data, size);
          }) {
        SPDLOG_INFO("创建视频流管理器 endpoint={}:{}",
                    endpoint.address().to_string(),
                    endpoint.port());
        video_shards_manager.set_reset_callback([this]() {
            stream_ready = false;
            if (video_decoder != nullptr) video_decoder->reset();
        });
        // 接收到完整的视频帧之后去解码
        video_shards_manager.set_frame_callback([this](const std::vector<uint8_t> &frame) {
            // 队友画面旁路：把官方图传的原始 HEVC 字节原样交给上层转发。
            // 只是入队，不阻塞，也不看 stream_ready——对方自己会等 IDR。
            if (this->raw_frame_callback) {
                this->raw_frame_callback(frame);
            }

            if (!stream_ready) {
                int nal_type = VideoDecoder::get_nal_type(frame);
                if (nal_type == -1) {
                    SPDLOG_DEBUG("尚未拿到可解码视频帧，忽略无效NAL frame_bytes={}", frame.size());
                    return;
                }
                bool is_param = (nal_type == 32 || nal_type == 33 || nal_type == 34);
                bool is_idr = (nal_type == 19 || nal_type == 20 || nal_type == 21);

                // 遇到IDR帧或已有参数集时开始流
                if (is_idr || is_param) {
                    stream_ready = true;
                    SPDLOG_INFO("视频流已就绪 nal_type={} frame_bytes={}", nal_type, frame.size());
                }
            }

            // 解码
            if (stream_ready) {
                video_decoder->decode_frame(frame);
            } else {
                SPDLOG_DEBUG("视频流尚未就绪，丢弃帧 bytes={}", frame.size());
            }
        });
        // 为视频解码单独创建一个线程
        decode_thread = std::thread([this, frame_callback]() {
            try {
                video_decoder = std::make_unique<VideoDecoder>(frame_callback);
                SPDLOG_INFO("视频解码线程已启动");
                while (is_running.load(std::memory_order_acquire)) {
                    try {
                        video_shards_manager.handle_once();
                    } catch (const std::exception &error) {
                        stream_ready = false;
                        SPDLOG_WARN("官方视频帧处理失败，等待流恢复 message={}", error.what());
                        video_decoder->reset();
                    }
                }
                SPDLOG_INFO("视频解码线程已退出");
            } catch (const std::exception &error) {
                try { SPDLOG_ERROR("官方视频解码线程已停止 message={}", error.what()); } catch (...) {}
            } catch (...) {
                try { SPDLOG_ERROR("官方视频解码线程出现未知异常，已停止"); } catch (...) {}
            }
            is_running.store(false, std::memory_order_release);
        });
    }

    VideoStreamManager::~VideoStreamManager() { stop(); }

    void VideoStreamManager::stop() {
        SPDLOG_INFO("收到视频流管理器停止请求");
        video_stream_receiver.stop();
        is_running.store(false, std::memory_order_relaxed);
        video_shards_manager.stop();
        if (decode_thread.joinable()) {
            decode_thread.join();
        }
        SPDLOG_INFO("视频流管理器已停止");
    }

}// namespace RoboGo::video
