#include "video_stream_receiver.h"
#include "logging/logging.h"

namespace RoboGo::io {

    VideoStreamReceiver::VideoStreamReceiver(boost::asio::io_context &io_context,
                                             const boost::asio::ip::udp::endpoint &endpoint,
                                             const std::function<void(const uint8_t *data, size_t size)> &on_receive_packet)
        : socket(io_context, endpoint),
          on_receive_packet(on_receive_packet) {
        socket.set_option(boost::asio::socket_base::receive_buffer_size(20 * 1024 * 1024));
        SPDLOG_INFO("创建视频流接收器 local_endpoint={} receive_buffer_bytes={}",
                    socket.local_endpoint().address().to_string() + ":" + std::to_string(socket.local_endpoint().port()),
                    20 * 1024 * 1024);
        boost::asio::co_spawn(io_context, receive_loop(), boost::asio::detached);
    }

    void VideoStreamReceiver::stop() {
        SPDLOG_INFO("收到视频流接收器停止请求");
        is_running.store(false, std::memory_order_relaxed);
        boost::system::error_code error_code;
        socket.close(error_code);
        if (error_code) {
            SPDLOG_ERROR("关闭视频流socket失败 message={}", error_code.message());
        }
    }

    boost::asio::awaitable<void> VideoStreamReceiver::receive_loop() {
        std::vector<uint8_t> buffer(65535);
        boost::asio::ip::udp::endpoint sender_endpoint;
        try {
            while (is_running.load(std::memory_order_relaxed)) {
                size_t length = co_await socket.async_receive_from(
                        boost::asio::buffer(buffer),
                        sender_endpoint,
                        boost::asio::use_awaitable);

                const std::string sender = sender_endpoint.address().to_string() + ":" + std::to_string(sender_endpoint.port());
                RoboGo::logging::log_network_payload("udp-video",
                                                     "rx",
                                                     "video-stream",
                                                     std::span<const uint8_t>(buffer.data(), length),
                                                     sender);
                on_receive_packet(buffer.data(), length);
            }
        } catch (const boost::system::system_error &error) {
            if (is_running.load(std::memory_order_relaxed)) {
                SPDLOG_ERROR("视频流接收异常 code={} message={}",
                             static_cast<long long>(error.code().value()),
                             error.code().message());
            } else {
                SPDLOG_INFO("视频流接收循环因停止请求退出 message={}", error.code().message());
            }
        }
        SPDLOG_INFO("视频流接收循环已退出");
    }

}// namespace RoboGo::io
