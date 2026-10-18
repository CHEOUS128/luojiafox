#include "mqtt_client.h"
#include "logging/logging.h"

namespace {

    using MqttAsyncClient = async_mqtt::client<async_mqtt::protocol_version::v3_1_1, async_mqtt::protocol::mqtt>;

    constexpr auto mqtt_operation_timeout = std::chrono::seconds(5);
    constexpr auto mqtt_reconnect_delay = std::chrono::milliseconds(100);
    constexpr std::uint16_t mqtt_keep_alive_seconds = 5;

    boost::asio::awaitable<void> WaitForMqttOperationTimeout() {
        auto executor = co_await boost::asio::this_coro::executor;
        boost::asio::steady_timer timer(executor, mqtt_operation_timeout);
        auto [error_code] = co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
        if (error_code) {
            throw boost::system::system_error(error_code);
        }
    }

    boost::asio::awaitable<void> CloseMqttClientNoThrow(MqttAsyncClient &client, std::string_view peer, std::string_view reason) {
        try {
            co_await client.async_close(boost::asio::use_awaitable);
        } catch (const boost::system::system_error &error) {
            if (error.code() != boost::asio::error::operation_aborted &&
                error.code() != boost::asio::error::not_connected &&
                error.code() != boost::asio::error::bad_descriptor) {
                SPDLOG_WARN("MQTT关闭连接失败 peer={} reason={} code={} message={}",
                            peer,
                            reason,
                            static_cast<long long>(error.code().value()),
                            error.code().message());
            }
        }
    }

    template <typename OperationFactory>
    boost::asio::awaitable<void> RunMqttOperationWithTimeout(MqttAsyncClient &client,
                                                             std::string_view peer,
                                                             std::string_view operation_name,
                                                             OperationFactory &&operation_factory,
                                                             bool close_client_on_timeout = true) {
        using namespace boost::asio::experimental::awaitable_operators;

        auto operation = std::forward<OperationFactory>(operation_factory)();
        auto result = co_await (std::move(operation) || WaitForMqttOperationTimeout());
        if (result.index() == 1) {
            const auto timeout_ms = std::chrono::duration_cast<std::chrono::milliseconds>(mqtt_operation_timeout).count();
            SPDLOG_WARN("MQTT操作超时 peer={} operation={} timeout_ms={}", peer, operation_name, timeout_ms);
            if (close_client_on_timeout) {
                co_await CloseMqttClientNoThrow(client, peer, operation_name);
            } else {
                SPDLOG_WARN("MQTT操作超时后不主动关闭连接 peer={} operation={}", peer, operation_name);
            }
            throw boost::system::system_error(boost::asio::error::timed_out);
        }
    }

    boost::asio::awaitable<void> PublishMqttMessage(std::shared_ptr<MqttAsyncClient> client,
                                                    std::string topic,
                                                    std::string message,
                                                    std::string peer) {
        try {
            const auto packet_id = client->acquire_unique_packet_id();
            if (!packet_id.has_value()) {
                SPDLOG_WARN("MQTT会话未就绪，跳过发送 topic={} bytes={} peer={}",
                            topic,
                            message.size(),
                            peer);
                co_return;
            }
            RoboGo::logging::log_network_payload("mqtt",
                                                 "tx",
                                                 topic,
                                                 message,
                                                 peer);
            SPDLOG_DEBUG("发送MQTT消息 topic={} bytes={} peer={}", topic, message.size(), peer);
            co_await RunMqttOperationWithTimeout(*client,
                                                 peer,
                                                 "publish",
                                                 [&]() -> boost::asio::awaitable<void> {
                                                     co_await client->async_publish(
                                                             async_mqtt::v3_1_1::publish_packet{
                                                                     *packet_id,
                                                                     topic,
                                                                     message,
                                                                     async_mqtt::qos::at_least_once},
                                                             boost::asio::use_awaitable);
                                                 },
                                                 false);
        } catch (const boost::system::system_error &error) {
            const auto error_code = static_cast<long long>(error.code().value());
            const std::string error_message = error.code().message();
            SPDLOG_ERROR("MQTT发送失败 topic={} bytes={} peer={} code={} message={}",
                         topic,
                         message.size(),
                         peer,
                         error_code,
                         error_message);
        }
    }

}// namespace

namespace RoboGo::io {

    MqttClient::MqttClient(boost::asio::io_context &io_context,
                           std::string host_ip,
                           std::string port,
                           const std::function<void(const std::string &topic, const std::string &payload)> &on_receive_message)
        : host_ip(std::move(host_ip)),
          port(std::move(port)),
          on_receive_message(on_receive_message) {
        SPDLOG_INFO("创建MQTT客户端 host={} port={}", this->host_ip, this->port);
        boost::asio::co_spawn(io_context, run(), boost::asio::detached);
    }

    void MqttClient::set_server_endpoint(std::string host_ip, std::string port) {
        if (host_ip.empty() || port.empty()) {
            SPDLOG_WARN("忽略空的MQTT服务端地址 host={} port={}", host_ip, port);
            return;
        }

        bool endpoint_changed = false;
        {
            std::lock_guard<std::mutex> lock(endpoint_mutex);
            if (this->host_ip != host_ip || this->port != port) {
                SPDLOG_INFO("更新MQTT服务端地址 old={}:{} new={}:{}",
                            this->host_ip,
                            this->port,
                            host_ip,
                            port);
                this->host_ip = std::move(host_ip);
                this->port = std::move(port);
                endpoint_changed = true;
            }
        }

        if (!endpoint_changed) {
            return;
        }

        endpoint_revision.fetch_add(1, std::memory_order_acq_rel);
        bool has_client_id = false;
        {
            std::lock_guard<std::mutex> lock(client_id_mutex);
            has_client_id = !client_id.empty();
        }
        connection_state.store(has_client_id ? ConnectionState::Connecting : ConnectionState::Idle, std::memory_order_release);
    }

    std::pair<std::string, std::string> MqttClient::get_server_endpoint() const {
        std::lock_guard<std::mutex> lock(endpoint_mutex);
        return {host_ip, port};
    }

    void MqttClient::set_client_id(std::string client_id) {
        SPDLOG_INFO("更新MQTT客户端ID old={} new={}", this->client_id, client_id);
        bool has_client_id = false;
        {
            std::lock_guard<std::mutex> lock(client_id_mutex);
            this->client_id = std::move(client_id);
            has_client_id = !this->client_id.empty();
        }
        connection_state.store(has_client_id ? ConnectionState::Connecting : ConnectionState::Idle, std::memory_order_release);
        is_client_id_updated.store(true, std::memory_order_release);
    }

    MqttClient::ConnectionState MqttClient::get_connection_state() const {
        return connection_state.load(std::memory_order_acquire);
    }

    std::string MqttClient::get_client_id() const {
        std::lock_guard<std::mutex> lock(client_id_mutex);
        return client_id;
    }

    void MqttClient::stop() {
        const auto [host, service_port] = get_server_endpoint();
        SPDLOG_INFO("收到MQTT停止请求 host={} port={}", host, service_port);
        is_running.store(false, std::memory_order_relaxed);
        connection_state.store(ConnectionState::Idle, std::memory_order_release);
    }

    boost::asio::awaitable<void> MqttClient::run() {
        auto executor = co_await boost::asio::this_coro::executor;
        while (is_running.load(std::memory_order_relaxed)) {
            const std::uint64_t endpoint_revision_snapshot = endpoint_revision.load(std::memory_order_acquire);
            std::string host_ip_cloned;
            std::string port_cloned;
            {
                std::lock_guard<std::mutex> lock(endpoint_mutex);
                host_ip_cloned = host_ip;
                port_cloned = port;
            }
            const std::string peer = host_ip_cloned + ":" + port_cloned;
            const auto is_endpoint_updated = [&]() {
                return endpoint_revision.load(std::memory_order_acquire) != endpoint_revision_snapshot;
            };
            bool need_reconnect = false;
            try {
                if (host_ip_cloned.empty() || port_cloned.empty()) {
                    connection_state.store(ConnectionState::Idle, std::memory_order_release);
                    SPDLOG_WARN("MQTT服务端地址为空，等待后重试");
                    boost::asio::steady_timer timer(executor, std::chrono::milliseconds(100));
                    co_await timer.async_wait(boost::asio::use_awaitable);
                    continue;
                }

                // 每次连接都创建新的 client 对象
                auto client = std::make_shared<MqttAsyncClient>(executor);
                client->set_pingresp_recv_timeout(mqtt_operation_timeout);

                {
                    std::lock_guard<std::mutex> lock(client_mutex);
                    active_client = client;
                }

                // 发送连接请求
                is_client_id_updated.store(false, std::memory_order_relaxed);
                std::string client_id_cloned;
                {
                    std::lock_guard<std::mutex> lock(client_id_mutex);
                    client_id_cloned = client_id;
                }
                if (client_id_cloned.empty()) {
                    connection_state.store(ConnectionState::Idle, std::memory_order_release);
                    SPDLOG_DEBUG("MQTT尚未设置客户端ID，等待后重试 peer={}", peer);
                    boost::asio::steady_timer timer(executor, std::chrono::milliseconds(100));
                    co_await timer.async_wait(boost::asio::use_awaitable);
                    continue;
                }
                if (connection_state.load(std::memory_order_acquire) != ConnectionState::Failed) {
                    connection_state.store(ConnectionState::Connecting, std::memory_order_release);
                }

                // 检查在准备连接过程中是否已经更新了客户端ID
                if (is_client_id_updated.load(std::memory_order_relaxed) || is_endpoint_updated()) {
                    SPDLOG_DEBUG("MQTT参数在连接前已更新，跳过本次连接 peer={}", peer);
                    continue;
                }

                // 底层网络协议握手
                SPDLOG_INFO("MQTT开始底层网络协议握手 peer={} client_id={}", peer, client_id_cloned);
                co_await RunMqttOperationWithTimeout(*client,
                                                     peer,
                                                     "underlying_handshake",
                                                     [&]() -> boost::asio::awaitable<void> {
                                                         co_await client->async_underlying_handshake(host_ip_cloned, port_cloned, boost::asio::use_awaitable);
                                                     });
                SPDLOG_INFO("MQTT底层网络协议握手成功 peer={} client_id={}", peer, client_id_cloned);

                // 检查在握手过程中是否已经更新了客户端ID
                if (is_client_id_updated.load(std::memory_order_relaxed) || is_endpoint_updated()) {
                    SPDLOG_INFO("MQTT握手完成后检测到参数变更，关闭当前连接 peer={}", peer);
                    co_await client->async_close(boost::asio::use_awaitable);
                    continue;
                }

                // 发送连接请求并接收回复
                SPDLOG_INFO("MQTT开始连接 peer={} client_id={}", peer, client_id_cloned);
                co_await RunMqttOperationWithTimeout(*client,
                                                     peer,
                                                     "connect",
                                                     [&]() -> boost::asio::awaitable<void> {
                                                         co_await client->async_start(
                                                                 async_mqtt::v3_1_1::connect_packet{
                                                                         true,
                                                                         mqtt_keep_alive_seconds,
                                                                         std::move(client_id_cloned),
                                                                         std::nullopt,
                                                                         "",
                                                                         ""},
                                                                 boost::asio::use_awaitable);
                                                     });
                SPDLOG_INFO("MQTT连接成功 peer={}", peer);

                // 检查在连接过程中是否已经更新了客户端ID
                if (is_client_id_updated.load(std::memory_order_relaxed) || is_endpoint_updated()) {
                    SPDLOG_INFO("MQTT连接后检测到参数变更，主动断开 peer={}", peer);
                    co_await client->async_disconnect(async_mqtt::v3_1_1::disconnect_packet{}, boost::asio::use_awaitable);
                    co_await client->async_close(boost::asio::use_awaitable);
                    continue;
                }

                // 发送要订阅的话题
                SPDLOG_INFO("MQTT开始订阅话题 peer={}", peer);
                std::vector<async_mqtt::topic_subopts> sub_entry{
                        {"GameStatus", async_mqtt::qos::at_most_once},
                        {"GlobalUnitStatus", async_mqtt::qos::at_most_once},
                        {"GlobalLogisticsStatus", async_mqtt::qos::at_most_once},
                        {"GlobalSpecialMechanism", async_mqtt::qos::at_most_once},
                        {"Event", async_mqtt::qos::at_most_once},
                        {"RobotInjuryStat", async_mqtt::qos::at_most_once},
                        {"RobotRespawnStatus", async_mqtt::qos::at_most_once},
                        {"RobotStaticStatus", async_mqtt::qos::at_most_once},
                        {"RobotDynamicStatus", async_mqtt::qos::at_most_once},
                        {"RobotModuleStatus", async_mqtt::qos::at_most_once},
                        {"RobotPosition", async_mqtt::qos::at_most_once},
                        {"Buff", async_mqtt::qos::at_most_once},
                        {"PenaltyInfo", async_mqtt::qos::at_most_once},
                        {"RobotPathPlanInfo", async_mqtt::qos::at_most_once},
                        {"RadarInfoToClient", async_mqtt::qos::at_most_once},
                        {"MapClickInfo", async_mqtt::qos::at_most_once},
                        {"CustomByteBlock", async_mqtt::qos::at_most_once},
                        {"TechCoreMotionStateSync", async_mqtt::qos::at_most_once},
                        {"RobotPerformanceSelectionSync", async_mqtt::qos::at_most_once},
                        {"DeployModeStatusSync", async_mqtt::qos::at_most_once},
                        {"RuneStatusSync", async_mqtt::qos::at_most_once},
                        {"SentryStatusSync", async_mqtt::qos::at_most_once},
                        {"DartSelectTargetStatusSync", async_mqtt::qos::at_most_once},
                        {"SentryCtrlResult", async_mqtt::qos::at_most_once},
                        {"AirSupportStatusSync", async_mqtt::qos::at_most_once}};
                co_await RunMqttOperationWithTimeout(*client,
                                                     peer,
                                                     "subscribe",
                                                     [&]() -> boost::asio::awaitable<void> {
                                                         co_await client->async_subscribe(
                                                                 async_mqtt::v3_1_1::subscribe_packet{
                                                                         *client->acquire_unique_packet_id(),
                                                                         async_mqtt::force_move(sub_entry)},
                                                                 boost::asio::use_awaitable);
                                                     });
                SPDLOG_INFO("MQTT话题订阅成功 peer={} topic_count={}", peer, 25);
                connection_state.store(ConnectionState::Connected, std::memory_order_release);

                // 检查在订阅过程中是否已经更新了客户端ID
                if (is_client_id_updated.load(std::memory_order_relaxed) || is_endpoint_updated()) {
                    connection_state.store(ConnectionState::Connecting, std::memory_order_release);
                    SPDLOG_INFO("MQTT订阅后检测到参数变更，主动断开 peer={}", peer);
                    co_await client->async_disconnect(async_mqtt::v3_1_1::disconnect_packet{}, boost::asio::use_awaitable);
                    co_await client->async_close(boost::asio::use_awaitable);
                    continue;
                }

                // 接收信息
                while (is_running.load(std::memory_order_relaxed)
                       && !is_client_id_updated.load(std::memory_order_relaxed)
                       && !is_endpoint_updated()) {
                    if (std::optional<async_mqtt::packet_variant> pv_opt = co_await client->async_recv(boost::asio::use_awaitable)) {
                        pv_opt->visit(
                                async_mqtt::overload{
                                        [this, &peer](async_mqtt::v3_1_1::publish_packet const &p) {
                                            // 接收到对方发送的信息
                                            const std::string topic = p.topic();
                                            const std::string payload = p.payload();
                                            RoboGo::logging::log_network_payload("mqtt",
                                                                                 "rx",
                                                                                 topic,
                                                                                 payload,
                                                                                 peer);
                                            on_receive_message(topic, payload);
                                        },
                                        [&](async_mqtt::v3_1_1::puback_packet const &packet) {
                                            // 对方接收到信息的回应
                                            SPDLOG_DEBUG("收到MQTT PUBACK peer={} packet_id={}", peer, packet.packet_id());
                                        },
                                        [&](auto const &) {
                                            SPDLOG_TRACE("收到非业务MQTT数据包 peer={}", peer);
                                        }});
                    }
                }

                // 发送断开连接请求
                SPDLOG_INFO("MQTT开始主动断开连接 peer={}", peer);
                co_await client->async_disconnect(async_mqtt::v3_1_1::disconnect_packet{}, boost::asio::use_awaitable);
                SPDLOG_INFO("MQTT断开连接成功 peer={}", peer);

                // 关闭底层socket，确保状态重置
                co_await client->async_close(boost::asio::use_awaitable);

            } catch (const boost::system::system_error &error) {
                SPDLOG_ERROR("MQTT运行异常 peer={} code={} message={}",
                             peer,
                             static_cast<long long>(error.code().value()),
                             error.code().message());
                if (is_running.load(std::memory_order_relaxed)) {
                    connection_state.store(ConnectionState::Failed, std::memory_order_release);
                }
                need_reconnect = true;
            } catch (const std::exception &error) {
                SPDLOG_ERROR("MQTT运行异常 peer={} message={}", peer, error.what());
                if (is_running.load(std::memory_order_relaxed)) {
                    connection_state.store(ConnectionState::Failed, std::memory_order_release);
                }
                need_reconnect = true;
            }

            {
                std::lock_guard<std::mutex> lock(client_mutex);
                active_client.reset();
            }
            SPDLOG_DEBUG("MQTT活动连接已清理 peer={}", peer);

            if (need_reconnect) {
                // 出现错误，等待 100ms 再重新连接
                SPDLOG_WARN("MQTT将在{}ms后尝试重连 peer={}",
                            std::chrono::duration_cast<std::chrono::milliseconds>(mqtt_reconnect_delay).count(),
                            peer);
                boost::asio::steady_timer timer(executor, mqtt_reconnect_delay);
                auto [error_code] = co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
                if (error_code) {
                    SPDLOG_ERROR("MQTT重连等待失败 peer={} message={}", peer, error_code.message());
                }
            }
        }
        connection_state.store(ConnectionState::Idle, std::memory_order_release);
        SPDLOG_INFO("MQTT运行循环已退出");
    }

    void MqttClient::publish(const std::string &topic, const std::string &message) {
        std::string host_ip_cloned;
        std::string port_cloned;
        {
            std::lock_guard<std::mutex> lock(endpoint_mutex);
            host_ip_cloned = host_ip;
            port_cloned = port;
        }
        const std::string peer = host_ip_cloned + ":" + port_cloned;
        if (connection_state.load(std::memory_order_acquire) != ConnectionState::Connected) {
            SPDLOG_DEBUG("MQTT尚未登录，跳过发送 topic={} bytes={} peer={}", topic, message.size(), peer);
            return;
        }

        // 获取当前活动的 client 对象的共享指针副本
        std::shared_ptr<MqttAsyncClient> client_copy;
        {
            std::lock_guard<std::mutex> lock(client_mutex);
            if (active_client) {
                client_copy = active_client;
            }
        }

        if (client_copy) {
            boost::asio::co_spawn(client_copy->get_executor(),
                                  PublishMqttMessage(client_copy, topic, message, peer),
                                  boost::asio::detached);
        } else {
            SPDLOG_WARN("MQTT客户端未连接，跳过发送 topic={} bytes={} peer={}", topic, message.size(), peer);
        }
    }

}// namespace RoboGo::io
