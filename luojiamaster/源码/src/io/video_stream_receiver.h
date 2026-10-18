#pragma once

#include <pch.h>

namespace RoboGo::io {

    class VideoStreamReceiver {
    private:
        boost::asio::ip::udp::socket socket;
        std::function<void(const uint8_t *data, size_t size)> on_receive_packet;
        std::atomic<bool> is_running = true;

    public:
        explicit VideoStreamReceiver(boost::asio::io_context &io_context,
                                     const boost::asio::ip::udp::endpoint &endpoint,
                                     const std::function<void(const uint8_t *data, size_t size)> &on_receive_packet);

        void stop();

    private:
        boost::asio::awaitable<void> receive_loop();
    };

}// namespace RoboGo::io
