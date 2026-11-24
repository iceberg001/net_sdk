/**
 * @file test_http_concurrency.cpp
 * @brief HTTP 并发模型回归测试（不依赖外网，使用本机 loopback）
 *
 * 覆盖：
 *  - 多个异步请求真正并发（不再被一把全局锁串行化）
 *  - 请求级 watchdog 超时：超时即回调 Timeout，迟到的响应被丢弃，恰好一次回调
 *  - 同 key 覆盖 requestAsyncLatest：旧请求立即 Superseded，新请求不等旧请求
 *  - cancel()：被取消请求回调一次 Canceled
 *
 * 本机 mini HTTP server 支持 /delay?ms=N，用于制造可控的慢响应。
 */
#include "test_util.h"
#include <net_sdk/NetSdk.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define closesocket ::close
using SOCKET = int;
#define INVALID_SOCKET (-1)
#endif

using namespace std::chrono;

namespace {

class MiniHttpServer {
public:
    explicit MiniHttpServer(int port) {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int on = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&on), sizeof(on));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<unsigned short>(port));
        ::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(listenFd_, 64);
        acceptThread_ = std::thread([this]() { acceptLoop(); });
    }

    ~MiniHttpServer() {
        stop_.store(true);
        closesocket(listenFd_);
        if (acceptThread_.joinable()) acceptThread_.join();
    }

private:
    void acceptLoop() {
        while (!stop_.load()) {
            SOCKET c = ::accept(listenFd_, nullptr, nullptr);
            if (c == INVALID_SOCKET) break;
            // 每个连接一个独立线程，模拟真实的并发慢响应；
            // 线程不访问 this，析构后仍可安全结束。
            std::thread([c]() { handle(c); }).detach();
        }
    }

    static void handle(SOCKET c) {
        char buf[4096];
        const int n = static_cast<int>(::recv(c, buf, sizeof(buf) - 1, 0));
        if (n <= 0) {
            closesocket(c);
            return;
        }
        buf[n] = '\0';
        std::string req(buf);
        int delayMs = 0;
        size_t pos = req.find("ms=");
        if (pos != std::string::npos) delayMs = std::atoi(req.c_str() + pos + 3);
        if (delayMs > 0) std::this_thread::sleep_for(milliseconds(delayMs));

        const std::string body = "OK:" + std::to_string(delayMs);
        const std::string resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + body;
        ::send(c, resp.c_str(), static_cast<int>(resp.size()), 0);
        closesocket(c);
    }

    SOCKET listenFd_ = INVALID_SOCKET;
    std::atomic<bool> stop_{false};
    std::thread acceptThread_;
};

struct Collector {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<net_sdk::NetResult> results;
    int count = 0;

    explicit Collector(int n) : results(n) {}

    void put(int index, const net_sdk::NetResult& r) {
        std::lock_guard<std::mutex> lk(mutex);
        results[index] = r;
        ++count;
        cv.notify_all();
    }

    bool waitFor(int expected, int ms) {
        std::unique_lock<std::mutex> lk(mutex);
        return cv.wait_for(lk, milliseconds(ms), [&]() { return count >= expected; });
    }
};

} // anonymous namespace

int main() {
    using namespace net_sdk;

    const int port = 25710;
    MiniHttpServer server(port);
    std::this_thread::sleep_for(milliseconds(120));
    const std::string base = "http://127.0.0.1:" + std::to_string(port) + "/delay?ms=";

    auto& sdk = NetSdk::getInstance();
    sdk.init(4, 2);
    Logger::getInstance().setConsoleOutput(false);
    sdk.setLogLevel(LogLevel::Warn);
    auto& http = sdk.getHttpClient();
    http.setConcurrency(4);

    // ---------- 1) 真并发：6 个 400ms 请求，不应被串行成 2400ms ----------
    {
        Collector col(6);
        auto t0 = steady_clock::now();
        for (int i = 0; i < 6; ++i) {
            HttpRequestConfig cfg;
            cfg.url = base + "400";
            cfg.method = "GET";
            cfg.timeoutMs = 5000;
            http.requestAsync(cfg, [&col, i](const NetResult& r) { col.put(i, r); });
        }
        const bool all = col.waitFor(6, 8000);
        const long long elapsed = duration_cast<milliseconds>(steady_clock::now() - t0).count();
        testutil::check(all, "concurrency: all 6 async requests completed");
        bool allOk = true;
        for (const auto& r : col.results) allOk = allOk && r.ok() && r.statusCode == 200;
        testutil::check(allOk, "concurrency: all responses are HTTP 200");
        // 全局串行需 ~2400ms；4 槽并发两批应 < 1800ms。
        testutil::check(elapsed < 1800,
                        "concurrency: no global serialization (elapsed=" +
                        std::to_string(elapsed) + "ms < 1800ms)");
    }

    // ---------- 2) 同 key 覆盖：旧请求立即 Superseded，新请求不等旧请求 ----------
    {
        Collector col(2);
        HttpRequestConfig slow;
        slow.url = base + "1200";
        slow.method = "GET";
        slow.timeoutMs = 5000;
        http.requestAsyncLatest("status", slow,
                                [&col](const NetResult& r) { col.put(0, r); });

        std::this_thread::sleep_for(milliseconds(100));

        HttpRequestConfig fast;
        fast.url = base + "100";
        fast.method = "GET";
        fast.timeoutMs = 5000;
        const auto t1 = steady_clock::now();
        http.requestAsyncLatest("status", fast,
                                [&col](const NetResult& r) { col.put(1, r); });

        const bool all = col.waitFor(2, 4000);
        const long long fastElapsed = duration_cast<milliseconds>(steady_clock::now() - t1).count();
        testutil::check(all, "latest: both callbacks delivered (old + new)");
        testutil::check(col.results[0].code == NetErrorCode::Superseded,
                        "latest: old request reported Superseded immediately");
        testutil::check(col.results[1].ok() && col.results[1].statusCode == 200,
                        "latest: new request succeeded");
        testutil::check(fastElapsed < 900,
                        "latest: new request did not wait for the stuck old one (elapsed=" +
                        std::to_string(fastElapsed) + "ms)");
    }

    // ---------- 3) cancel：被取消的请求回调一次 Canceled ----------
    {
        Collector col(1);
        HttpRequestConfig cfg;
        cfg.url = base + "1500";
        cfg.method = "GET";
        cfg.timeoutMs = 5000;
        const uint64_t id = http.requestAsync(cfg,
            [&col](const NetResult& r) { col.put(0, r); });
        std::this_thread::sleep_for(milliseconds(60));
        const bool hit = http.cancel(id);
        col.waitFor(1, 2000);
        testutil::check(hit, "cancel: cancel() found the in-flight request");
        testutil::check(col.results[0].code == NetErrorCode::Canceled,
                        "cancel: callback reported Canceled");
        testutil::check(col.count == 1, "cancel: exactly one callback");
    }

    // ---------- 4) 请求级超时：300ms 到期即回调，迟到响应被丢弃 ----------
    {
        Collector col(1);
        HttpRequestConfig cfg;
        cfg.url = base + "2000";
        cfg.method = "GET";
        cfg.timeoutMs = 300;
        const auto t0 = steady_clock::now();
        http.requestAsync(cfg, [&col](const NetResult& r) { col.put(0, r); });
        const bool got = col.waitFor(1, 2000);
        const long long elapsed = duration_cast<milliseconds>(steady_clock::now() - t0).count();
        testutil::check(got, "timeout: callback delivered without waiting for slow server");
        testutil::check(col.results[0].code == NetErrorCode::Timeout,
                        "timeout: callback reported Timeout");
        testutil::check(elapsed < 1200,
                        "timeout: fired near configured 300ms (elapsed=" +
                        std::to_string(elapsed) + "ms)");

        // 等服务端那 2 秒的响应真正到达，确认不会二次回调。
        std::this_thread::sleep_for(milliseconds(2400));
        testutil::check(col.count == 1,
                        "timeout: late network response discarded (still exactly one callback)");
    }

    sdk.shutdown();
    // 让 server 的 detached 慢连接线程自然收尾（不访问已析构对象）。
    std::this_thread::sleep_for(milliseconds(100));
    return testutil::finish("http_concurrency");
}
