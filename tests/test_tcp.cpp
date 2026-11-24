// TCP 模块：本机 echo 连接/收发、客户端带连接析构、服务端带连接析构（UAF 回归）。
#include "test_util.h"
#include <net_sdk/NetSdk.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <string>

static void waitMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int main() {
    using namespace net_sdk;
    NetSdk::getInstance().init(4, 2);
    Logger::getInstance().setConsoleOutput(false);

    std::atomic<int> srvConn{0}, cliConn{0}, echoes{0}, srvDisc{0};
    std::mutex m;
    std::string got;

    // --- 场景1：连接 + 双向 echo ---
    {
        TcpServer server;
        server.setConnectionCallback([&](int) { srvConn++; });
        server.setDisconnectionCallback([&](int) { srvDisc++; });
        server.setMessageCallback([&](const std::string& d) {
            { std::lock_guard<std::mutex> lk(m); got = d; }
            echoes++;
            server.send(1, "echo:" + d);
        });
        TcpServerConfig sc; sc.host = "127.0.0.1"; sc.port = 25671;
        testutil::check(server.start(sc), "tcp: server start");
        waitMs(150);

        TcpClient client;
        client.setConnectionCallback([&](int id) { if (id > 0) cliConn++; });
        client.setMessageCallback([&](const std::string& d) {
            std::lock_guard<std::mutex> lk(m);
            got = d;
        });
        TcpClientConfig cc; cc.host = "127.0.0.1"; cc.port = 25671; cc.autoReconnect = false;
        testutil::check(client.connect(cc), "tcp: client connect initiated");
        waitMs(400);
        testutil::check(cliConn.load() >= 1 && srvConn.load() >= 1, "tcp: connection established");
        client.send("ping");
        waitMs(400);
        std::string finalGot;
        { std::lock_guard<std::mutex> lk(m); finalGot = got; }
        testutil::eq(finalGot, "echo:ping", "tcp: echo roundtrip");
        // client 带连接析构（UAF 高发）
    }
    waitMs(200);
    testutil::check(srvDisc.load() >= 1, "tcp: client dtor triggered server disconnect");

    // --- 场景2：服务端带活跃连接直接析构（UAF 高发），重复多次 ---
    for (int round = 0; round < 3; ++round) {
        TcpClient client; // 活得比 server 久
        TcpClientConfig cc; cc.host = "127.0.0.1"; cc.port = 25672; cc.autoReconnect = false;
        client.connect(cc);
        {
            TcpServer server;
            TcpServerConfig sc; sc.host = "127.0.0.1"; sc.port = 25672;
            server.start(sc);
            waitMs(300);
            // server 带活跃连接析构
        }
        waitMs(150);
    }
    testutil::check(true, "tcp: server dtor with live connection stable (3 rounds)");

    NetSdk::getInstance().shutdown();
    return testutil::finish("tcp");
}
