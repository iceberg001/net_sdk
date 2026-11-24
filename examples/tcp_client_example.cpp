#include "net_sdk/NetSdk.h"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    auto& sdk = net_sdk::NetSdk::getInstance();
    sdk.init();
    sdk.setLogLevel(net_sdk::LogLevel::Debug);
    sdk.enableFileLog(true, "app");  // 生成 YYYY-MM-DD_app.log，超过 5MB 依次轮转为 app1.log、app2.log ...

    std::cout << "=== TCP Client Example ===" << std::endl;

    {
        // client 必须在 sdk.shutdown() 之前析构：析构时会向绑定的 loop 投递
        // 清理事件，若 loop 已被 shutdown 停止，投递会访问已失效资源。
        net_sdk::TcpClient client;
        client.setEventLoopPool(&sdk.getEventLoopPool());

        client.setMessageCallback([](const std::string& data) {
            std::cout << "Received: " << data << std::endl;
        });

        client.setConnectionCallback([](int connId) {
            if (connId > 0) {
                std::cout << "Connected, connId=" << connId << std::endl;
            } else {
                std::cout << "Disconnected" << std::endl;
            }
        });

        client.setErrorCallback([](int connId, net_sdk::NetErrorCode code, const std::string& msg) {
            std::cout << "Error: " << msg << std::endl;
        });

        net_sdk::TcpClientConfig config;
        config.host = "127.0.0.1";
        config.port = 12345;
        config.autoReconnect = true;
        config.reconnectIntervalMs = 3000;

        if (client.connect(config)) {
            std::cout << "Connecting..." << std::endl;

            std::this_thread::sleep_for(std::chrono::seconds(2));

            if (client.isConnected()) {
                client.send("Hello from TCP client!");
            }

            std::this_thread::sleep_for(std::chrono::seconds(5));
        }

        client.disconnect();
    } // client 析构，loop 仍有效

    sdk.shutdown();
    return 0;
}
