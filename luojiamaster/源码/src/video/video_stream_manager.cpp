#include "video_stream_manager.h"

namespace RoboGo::video {

    VideoStreamManager::VideoStreamManager(boost::asio::io_context &io_context,
                                           boost::asio::ip::udp::endpoint endpoint,
                                           const std::function<void(const AVFrame *frame)> &frame_callback)
        : video_stream_receiver(io_context, endpoint, [this](const uint8_t *data, size_t size) {
              // 接收到数据包之后送入分片管理
              video_shards_manager.on_receive_shard(data, size);
          }),
          video_decoder(frame_callback) {
        SPDLOG_INFO("创建视频流管理器 endpoint={}:{}",
                    endpoint.address().to_string(),
                    endpoint.port());
        // 接收到完整的视频帧之后去解码
        video_shards_manager.set_frame_callback([this](const std::vector<uint8_t> &frame) {
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
                video_decoder.decode_frame(frame);
            } else {
                SPDLOG_DEBUG("视频流尚未就绪，丢弃帧 bytes={}", frame.size());
            }
        });
        // 为视频解码单独创建一个线程
        decode_thread = std::thread([this]() {
            SPDLOG_INFO("视频解码线程已启动");
            while (is_running.load(std::memory_order_relaxed)) {
                video_shards_manager.handle_once();
            }
            SPDLOG_INFO("视频解码线程已退出");
        });
    }

    void VideoStreamManager::stop() {
        SPDLOG_INFO("收到视频流管理器停止请求");
        video_stream_receiver.stop();
        is_running.store(false, std::memory_order_relaxed);
        if (decode_thread.joinable()) {
            decode_thread.join();
        }
        SPDLOG_INFO("视频流管理器已停止");
    }

}// namespace RoboGo::video
