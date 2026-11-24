// TCP 并发/超时/覆盖/取消/背压/断连 + 帧边界 + 服务端多 worker loop 回归测试。
// 全部使用本机 loopback，不依赖外网。
#include "test_util.h"
#include <net_sdk/NetSdk.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace net_sdk;

static void waitMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static long long msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t).count();
}

static bool waitConnected(TcpClient& c, int timeoutMs) {
    auto t0 = std::chrono::steady_clock::now();
    while (!c.isConnected() && msSince(t0) < timeoutMs) waitMs(10);
    return c.isConnected();
}

// 收集请求回调结果（回调在事件循环线程，主线程等待）
struct Recorder {
    std::mutex m;
    std::condition_variable cv;
    std::vector<NetErrorCode> codes;
    std::vector<std::string> bodies;

    void on(const NetResult& r) {
        std::lock_guard<std::mutex> lk(m);
        codes.push_back(r.code);
        bodies.push_back(r.body);
        cv.notify_all();
    }
    bool waitFor(size_t n, int timeoutMs) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                           [&] { return codes.size() >= n; });
    }
    size_t count() {
        std::lock_guard<std::mutex> lk(m);
        return codes.size();
    }
    NetErrorCode at(size_t i) {
        std::lock_guard<std::mutex> lk(m);
        return codes.at(i);
    }
    bool allAre(NetErrorCode c) {
        std::lock_guard<std::mutex> lk(m);
        if (codes.empty()) return false;
        for (auto code : codes) if (code != c) return false;
        return true;
    }
};

// 帧协议（按 '\n' 分帧）：
//   请求 "req:<id>:<delayMs>\n" -> 回包 "rsp:<id>\n"
//   "noreply:<id>\n" 故意不回包
//   "cli:<client>:<req>\n"     -> 广播 "rsp:<client>:<req>\n"
// 延迟回包用独立线程，避免阻塞服务端 loop，从而验证「多请求同时在途」。
static void startEchoServer(TcpServer& server, int port, int threadNum,
                            std::vector<std::thread>& delayed, std::mutex& delayedMtx) {
    server.setLineFrameMode('\n');
    server.setMessageCallback([&server, &delayed, &delayedMtx](const std::string& d) {
        if (d.rfind("req:", 0) == 0) {
            auto pos = d.find(':', 4);
            if (pos == std::string::npos) return;
            std::string id = d.substr(4, pos - 4);
            int delay = 0;
            try { delay = std::stoi(d.substr(pos + 1)); } catch (...) { delay = 0; }
            if (delay <= 0) {
                server.broadcast("rsp:" + id + "\n");
            } else {
                std::lock_guard<std::mutex> lk(delayedMtx);
                delayed.emplace_back([&server, id, delay]() {
                    waitMs(delay);
                    server.broadcast("rsp:" + id + "\n");
                });
            }
        } else if (d.rfind("cli:", 0) == 0) {
            server.broadcast("rsp:" + d.substr(4) + "\n");
        }
    });
    TcpServerConfig sc;
    sc.host = "127.0.0.1";
    sc.port = port;
    sc.threadNum = threadNum;
    sc.readTimeoutMs = 120000;
    testutil::check(server.start(sc), "tcp: echo server start (threadNum="
        + std::to_string(threadNum) + ")");
    waitMs(150);
}

static void joinDelayed(std::vector<std::thread>& delayed, std::mutex& delayedMtx) {
    std::vector<std::thread> toJoin;
    {
        std::lock_guard<std::mutex> lk(delayedMtx);
        toJoin.swap(delayed);
    }
    for (auto& t : toJoin) if (t.joinable()) t.join();
}

int main() {
    NetSdk::getInstance().init(4, 2);
    Logger::getInstance().setConsoleOutput(false);

    // ================= A. 单客户端：请求级并发 / 超时 / 覆盖 / 取消 / 背压 / 断连 =================
    {
        std::vector<std::thread> delayed;
        std::mutex delayedMtx;
        TcpServer server;
        startEchoServer(server, 25701, 1, delayed, delayedMtx);

        TcpClient client;
        client.setLineFrameMode('\n');
        TcpClientConfig cc;
        cc.host = "127.0.0.1";
        cc.port = 25701;
        cc.autoReconnect = false;
        cc.readTimeoutMs = 60000;
        cc.writeTimeoutMs = 60000;
        testutil::check(client.connect(cc), "tcp: client connect initiated");
        testutil::check(waitConnected(client, 3000), "tcp: client connected");

        // --- A1: 6 个请求同时在途，服务端各延迟 300ms 回包 ---
        {
            Recorder rec;
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 6; ++i) {
                std::string id = "c" + std::to_string(i);
                uint64_t rid = client.sendRequest("req:" + id + ":300\n", 3000,
                    [&rec](const NetResult& r) { rec.on(r); },
                    [id](const std::string& f) { return f == "rsp:" + id; });
                testutil::check(rid > 0, "tcp: concurrent request #" + id + " accepted");
            }
            testutil::check(rec.waitFor(6, 5000), "tcp: 6 concurrent responses received");
            long long elapsed = msSince(t0);
            testutil::check(rec.count() == 6 && rec.allAre(NetErrorCode::Ok),
                "tcp: all concurrent responses are Ok");
            testutil::check(elapsed < 1500,
                "tcp: 6x300ms requests ran concurrently (elapsed="
                + std::to_string(elapsed) + "ms, serial would be 1800ms)");
        }

        // --- A2: 请求级 watchdog 超时，且恰好一次回调 ---
        {
            Recorder rec;
            client.sendRequest("noreply:t1\n", 250,
                [&rec](const NetResult& r) { rec.on(r); });
            testutil::check(rec.waitFor(1, 1500), "tcp: timeout callback fired");
            testutil::check(rec.count() == 1 && rec.at(0) == NetErrorCode::Timeout,
                "tcp: timeout code == Timeout");
            waitMs(300);
            testutil::check(rec.count() == 1, "tcp: timeout callback fired exactly once");
        }

        // --- A3: 迟到回包被丢弃，不产生二次回调 ---
        {
            Recorder rec;
            client.sendRequest("req:late:600\n", 200,
                [&rec](const NetResult& r) { rec.on(r); },
                [](const std::string& f) { return f == "rsp:late"; });
            testutil::check(rec.waitFor(1, 1000), "tcp: late-response timeout fired");
            waitMs(700);  // 等服务端迟到回包到达
            testutil::check(rec.count() == 1 && rec.at(0) == NetErrorCode::Timeout,
                "tcp: late response discarded, no second callback");
        }

        // --- A4: 同 key 覆盖：旧请求立即 Superseded，新请求不等旧请求 ---
        {
            Recorder oldRec, newRec;
            uint64_t oldId = client.sendRequestLatest("heartbeat", "req:oldhb:800\n", 3000,
                [&oldRec](const NetResult& r) { oldRec.on(r); },
                [](const std::string& f) { return f == "rsp:oldhb"; });
            testutil::check(oldId > 0, "tcp: latest(old) request accepted");
            waitMs(100);
            auto t0 = std::chrono::steady_clock::now();
            client.sendRequestLatest("heartbeat", "req:newhb:100\n", 3000,
                [&newRec](const NetResult& r) { newRec.on(r); },
                [](const std::string& f) { return f == "rsp:newhb"; });
            testutil::check(oldRec.waitFor(1, 600), "tcp: old request superseded promptly");
            testutil::check(oldRec.count() == 1 && oldRec.at(0) == NetErrorCode::Superseded,
                "tcp: superseded code == Superseded");
            testutil::check(newRec.waitFor(1, 2000), "tcp: new request got response");
            testutil::check(newRec.count() == 1 && newRec.at(0) == NetErrorCode::Ok,
                "tcp: new request ok");
            testutil::check(msSince(t0) < 1200,
                "tcp: new request not blocked by old pending request");
            waitMs(800);  // 等旧请求的迟到回包
            testutil::check(oldRec.count() == 1 && newRec.count() == 1,
                "tcp: superseded request late response ignored");
        }

        // --- A5: cancel 生效 ---
        {
            Recorder rec;
            uint64_t rid = client.sendRequest("req:cx:800\n", 3000,
                [&rec](const NetResult& r) { rec.on(r); },
                [](const std::string& f) { return f == "rsp:cx"; });
            testutil::check(rid > 0, "tcp: cancel target request accepted");
            waitMs(100);
            testutil::check(client.cancelRequest(rid), "tcp: cancelRequest accepted");
            testutil::check(rec.waitFor(1, 600), "tcp: cancel callback fired");
            testutil::check(rec.count() == 1 && rec.at(0) == NetErrorCode::Canceled,
                "tcp: canceled code == Canceled");
            waitMs(900);
            testutil::check(rec.count() == 1, "tcp: canceled request fired exactly once");
        }

        // --- A6: 在途上限背压 ---
        {
            client.setMaxPendingRequests(2);
            Recorder rec;
            uint64_t a = client.sendRequest("noreply:b1\n", 5000,
                [&rec](const NetResult& r) { rec.on(r); });
            uint64_t b = client.sendRequest("noreply:b2\n", 5000,
                [&rec](const NetResult& r) { rec.on(r); });
            uint64_t c = client.sendRequest("noreply:b3\n", 5000,
                [&rec](const NetResult& r) { rec.on(r); });
            testutil::check(a > 0 && b > 0, "tcp: first two requests within limit");
            testutil::check(c == 0, "tcp: third request rejected by backpressure");
            client.cancelAllRequests();
            testutil::check(rec.waitFor(2, 1000), "tcp: cancelAllRequests callbacks fired");
            testutil::check(rec.count() == 2 && rec.allAre(NetErrorCode::Canceled),
                "tcp: all pending canceled");
            testutil::check(client.getPendingRequestCount() == 0,
                "tcp: pending count back to zero");
            client.setMaxPendingRequests(256);
        }

        // --- A7: 断连时在途请求立即 ConnectionClosed，不干等 watchdog ---
        {
            Recorder rec;
            client.sendRequest("noreply:dc\n", 5000,
                [&rec](const NetResult& r) { rec.on(r); });
            waitMs(50);
            auto t0 = std::chrono::steady_clock::now();
            client.disconnect();
            testutil::check(rec.waitFor(1, 1000), "tcp: disconnect fails pending request");
            testutil::check(rec.count() == 1 && rec.at(0) == NetErrorCode::ConnectionClosed,
                "tcp: pending failed with ConnectionClosed");
            testutil::check(msSince(t0) < 1500,
                "tcp: pending failed fast instead of waiting 5s watchdog");
        }

        // 延迟回包线程必须在 server 析构前回收。
        joinDelayed(delayed, delayedMtx);
    }   // client -> server 依次析构

    // ================= B. 服务端 threadNum=4：多连接并发 + send/broadcast UAF 回归 =================
    {
        TcpServer server;
        std::vector<std::thread> delayed;   // 本段无延迟请求，仅复用启动函数
        std::mutex delayedMtx;
        startEchoServer(server, 25702, 4, delayed, delayedMtx);

        std::vector<std::unique_ptr<TcpClient>> clients;
        for (int i = 0; i < 4; ++i) {
            auto c = std::make_unique<TcpClient>();
            c->setLineFrameMode('\n');
            TcpClientConfig cc;
            cc.host = "127.0.0.1";
            cc.port = 25702;
            cc.autoReconnect = false;
            cc.readTimeoutMs = 60000;
            c->connect(cc);
            clients.push_back(std::move(c));
        }
        bool allConnected = true;
        for (auto& c : clients) allConnected = waitConnected(*c, 3000) && allConnected;
        testutil::check(allConnected, "tcp: 4 clients connected to multi-worker server");
        testutil::check(server.getConnectionCount() == 4,
            "tcp: server tracks all 4 connections");

        Recorder rec;
        for (int i = 0; i < 4; ++i) {
            std::string cid = "c" + std::to_string(i);
            clients[i]->sendRequest("cli:" + cid + ":r0\n", 3000,
                [&rec](const NetResult& r) { rec.on(r); },
                [cid](const std::string& f) { return f == "rsp:" + cid + ":r0"; });
        }
        testutil::check(rec.waitFor(4, 4000), "tcp: 4 clients got responses concurrently");
        testutil::check(rec.count() == 4 && rec.allAre(NetErrorCode::Ok),
            "tcp: all 4 multi-worker responses are Ok");

        // 连接关闭后再 send/broadcast/closeConnection，验证不会写悬空 hio_t*。
        clients[0]->disconnect();
        waitMs(200);
        server.send(1, "after-close\n");
        server.broadcast("after-close\n");
        server.closeConnection(1);
        waitMs(200);
        testutil::check(true, "tcp: send/broadcast after connection close did not crash");

        for (auto& c : clients) c->disconnect();
        waitMs(150);
        joinDelayed(delayed, delayedMtx);
    }

    NetSdk::getInstance().shutdown();
    return testutil::finish("tcp_concurrency");
}
