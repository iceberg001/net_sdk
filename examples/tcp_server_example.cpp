#include "net_sdk/NetSdk.h"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    auto& sdk = net_sdk::NetSdk::getInstance();
    sdk.init();
    sdk.setLogLevel(net_sdk::LogLevel::Debug);
    sdk.enableFileLog(true, "app");  // 生成 YYYY-MM-DD_app.log，超过 5MB 依次轮转为 app1.log、app2.log ...

    std::cout << "=== TCP Server Example ===" << std::endl;

    net_sdk::TcpServer server;
    server.setEventLoopPool(&sdk.getEventLoopPool());

    server.setMessageCallback([](const std::string& data) {
        std::cout << "Received from client: " << data << std::endl;
    });

    server.setConnectionCallback([](int connId) {
        std::cout << "New connection: connId=" << connId << std::endl;
    });

    server.setDisconnectionCallback([](int connId) {
        std::cout << "Connection closed: connId=" << connId << std::endl;
    });

    net_sdk::TcpServerConfig config;
    config.host = "0.0.0.0";
    config.port = 12345;
    config.threadNum = 4;

    if (server.start(config)) {
        std::cout << "Server started on port 12345" << std::endl;
        std::cout << "Waiting for connections..." << std::endl;

        std::this_thread::sleep_for(std::chrono::seconds(30));

        std::cout << "Connection count: " << server.getConnectionCount() << std::endl;
        server.broadcast("Broadcast message to all clients");

        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    server.stop();
    sdk.shutdown();
    return 0;
}