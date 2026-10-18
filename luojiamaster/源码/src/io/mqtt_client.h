#pragma once

#include <pch.h>

namespace RoboGo::io {

    class MqttClient {
    public:
        enum class ConnectionState {
            Idle,
            Connecting,
            Connected,
            Failed,
        };

    private:
        mutable std::mutex endpoint_mutex;
        std::string host_ip;
        std::string port;
        std::atomic<std::uint64_t> endpoint_revision = 0;
        std::function<void(const std::string &topic, const std::string &payload)> on_receive_message;
        std::atomic<bool> is_running = true;
        mutable std::mutex client_id_mutex;
        std::string client_id;
        std::atomic<bool> is_client_id_updated = true;
        std::atomic<ConnectionState> connection_state = ConnectionState::Idle;
        // 用于管理当前活动的 client 对象
        std::mutex client_mutex;
        std::shared_ptr<async_mqtt::client<async_mqtt::protocol_version::v3_1_1, async_mqtt::protocol::mqtt>> active_client;

    public:
        explicit MqttClient(boost::asio::io_context &io_context,
                            std::string host_ip,
                            std::string port,
                            const std::function<void(const std::string &topic, const std::string &payload)> &on_receive_message);

        void set_server_endpoint(std::string host_ip, std::string port);

        [[nodiscard]] std::pair<std::string, std::string> get_server_endpoint() const;

        void set_client_id(std::string client_id);

        [[nodiscard]] ConnectionState get_connection_state() const;

        [[nodiscard]] std::string get_client_id() const;

        void stop();

    private:
        boost::asio::awaitable<void> run();

    public:
        void publish(const std::string &topic, const std::string &message);
    };


}// namespace RoboGo::io
