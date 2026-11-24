/**
 * @file test_callback_poster.cpp
 * @brief 宿主回调投递器 / 模板解析一体化 回归测试（不依赖外网，本机 loopback）
 *
 * 覆盖 v1.3.0 修复的核心问题：
 *  - A0：未注入 poster 时回调默认在 SDK 内部泵线程，**不在**宿主 UI 主线程
 *        （复现历史上「接口返回了但上层收不到回调」的根因环境）
 *  - A/B：注入 poster 后，成功 / 超时 / 取消 / 覆盖 的所有回调都在宿主 UI
 *         线程执行，且恰好一次（不静默丢弃 → 不会卡在「登录中」）
 *  - C：requestAsyncTyped<T> 在工作线程完成解析、把强类型结果回调到宿主
 *       主线程；超时等错误终态同样会回调，业务对象携带错误状态
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
#include <functional>
#include <mutex>
#include <queue>
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
            std::thread([c]() { handle(c); }).detach();
        }
    }

    static void handle(SOCKET c) {
        char buf[4096];
        const int n = static_cast<int>(::recv(c, buf, sizeof(buf) - 1, 0));
        if (n <= 0) { closesocket(c); return; }
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

/// 模拟宿主 UI 消息循环：专用线程 + 线程安全任务队列
class HostUiThread {
public:
    HostUiThread() {
        thread_ = std::thread([this]() { loop(); });
        uiId_ = thread_.get_id();
    }
    ~HostUiThread() {
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    void post(std::function<void()> task) {
        { std::lock_guard<std::mutex> lk(m_); q_.push(std::move(task)); }
        cv_.notify_one();
    }
    bool isHostThread() const { return std::this_thread::get_id() == uiId_; }
    void waitIdle(int ms) {
        const auto deadline = steady_clock::now() + milliseconds(ms);
        while (steady_clock::now() < deadline) {
            { std::lock_guard<std::mutex> lk(m_); if (q_.empty() && busy_ == 0) return; }
            std::this_thread::sleep_for(milliseconds(2));
        }
    }
private:
    void loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this]() { return stop_ || !q_.empty(); });
                if (stop_ && q_.empty()) return;
                task = std::move(q_.front());
                q_.pop();
                ++busy_;
            }
            if (task) { try { task(); } catch (...) {} }
            { std::lock_guard<std::mutex> lk(m_); --busy_; }
        }
    }
    std::thread thread_;
    std::thread::id uiId_;
    std::mutex m_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> q_;
    int busy_ = 0;
    bool stop_ = false;
};

struct Rec {
    std::mutex m;
    std::condition_variable cv;
    int count = 0;
    bool allOnHost = true;
    net_sdk::NetErrorCode lastCode = net_sdk::NetErrorCode::Ok;
    std::vector<net_sdk::NetErrorCode> codes;

    void on(const net_sdk::NetResult& r, bool host) {
        std::lock_guard<std::mutex> lk(m);
        ++count;
        if (!host) allOnHost = false;
        lastCode = r.code;
        codes.push_back(r.code);
        cv.notify_all();
    }
    bool waitFor(int n, int ms) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, milliseconds(ms), [&]() { return count >= n; });
    }
};

struct Demo {  // 业务响应结构体
    int code = -99;
    std::string body;
};

struct TypedRec {
    std::mutex m;
    std::condition_variable cv;
    int count = 0;
    bool allOnHost = true;
    int code = -99;
    std::string body;

    void on(const Demo& d, bool host) {
        std::lock_guard<std::mutex> lk(m);
        ++count;
        if (!host) allOnHost = false;
        code = d.code;
        body = d.body;
        cv.notify_all();
    }
    bool waitFor(int n, int ms) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, milliseconds(ms), [&]() { return count >= n; });
    }
};

} // anonymous namespace

int main() {
    using namespace net_sdk;

    const int port = 25720;
    MiniHttpServer server(port);
    std::this_thread::sleep_for(milliseconds(120));
    const std::string base = "http://127.0.0.1:" + std::to_string(port) + "/delay?ms=";

    auto& sdk = NetSdk::getInstance();
    sdk.init(4, 2);
    Logger::getInstance().setConsoleOutput(false);
    sdk.setLogLevel(LogLevel::Warn);
    auto& http = sdk.getHttpClient();

    HostUiThread host;

    // ---------- A0：未注入 poster → 回调不在宿主 UI 线程（根因环境） ----------
    {
        Rec rec;
        HttpRequestConfig cfg;
        cfg.url = base + "80"; cfg.method = "GET"; cfg.timeoutMs = 5000;
        http.requestAsync(cfg, [&rec, &host](const NetResult& r) {
            rec.on(r, host.isHostThread());
        });
        testutil::check(rec.waitFor(1, 4000), "poster(A0): default callback delivered");
        testutil::check(rec.count == 1, "poster(A0): exactly one callback");
        testutil::check(!rec.allOnHost,
                        "poster(A0): default callback is NOT on host UI thread (root cause)");
    }

    // ---------- 注入宿主投递器（宿主只配置一次，之后所有接口自动生效） ----------
    sdk.setCallbackPoster([&host](std::function<void()> task) {
        host.post(std::move(task));
    });

    // ---------- A：成功回调切到宿主主线程 ----------
    {
        Rec rec;
        HttpRequestConfig cfg;
        cfg.url = base + "60"; cfg.method = "GET"; cfg.timeoutMs = 5000;
        http.requestAsync(cfg, [&rec, &host](const NetResult& r) {
            rec.on(r, host.isHostThread());
        });
        testutil::check(rec.waitFor(1, 4000), "poster(A): success callback delivered");
        testutil::check(rec.count == 1, "poster(A): exactly one callback");
        testutil::check(rec.allOnHost, "poster(A): success callback runs on host UI thread");
        testutil::check(rec.lastCode == NetErrorCode::Ok, "poster(A): success code Ok");
    }

    // ---------- B1：超时终态也回调到宿主线程 ----------
    {
        Rec rec;
        HttpRequestConfig cfg;
        cfg.url = base + "900"; cfg.method = "GET"; cfg.timeoutMs = 200;
        http.requestAsync(cfg, [&rec, &host](const NetResult& r) {
            rec.on(r, host.isHostThread());
        });
        testutil::check(rec.waitFor(1, 2000), "poster(B1): timeout callback delivered");
        testutil::check(rec.lastCode == NetErrorCode::Timeout, "poster(B1): code Timeout");
        testutil::check(rec.allOnHost, "poster(B1): timeout callback on host UI thread");
        std::this_thread::sleep_for(milliseconds(1000));
        testutil::check(rec.count == 1, "poster(B1): late response discarded, still one callback");
    }

    // ---------- B2：取消终态也回调到宿主线程 ----------
    {
        Rec rec;
        HttpRequestConfig cfg;
        cfg.url = base + "1500"; cfg.method = "GET"; cfg.timeoutMs = 5000;
        const uint64_t id = http.requestAsync(cfg, [&rec, &host](const NetResult& r) {
            rec.on(r, host.isHostThread());
        });
        std::this_thread::sleep_for(milliseconds(60));
        const bool hit = http.cancel(id);
        testutil::check(hit, "poster(B2): cancel() found in-flight request");
        testutil::check(rec.waitFor(1, 2000), "poster(B2): canceled callback delivered");
        testutil::check(rec.lastCode == NetErrorCode::Canceled, "poster(B2): code Canceled");
        testutil::check(rec.allOnHost, "poster(B2): canceled callback on host UI thread");
        testutil::check(rec.count == 1, "poster(B2): exactly one callback");
    }

    // ---------- B3：同 key 覆盖，旧/新请求都回调到宿主线程 ----------
    {
        Rec oldRec, newRec;
        HttpRequestConfig slow;
        slow.url = base + "1200"; slow.method = "GET"; slow.timeoutMs = 5000;
        http.requestAsyncLatest("status", slow, [&oldRec, &host](const NetResult& r) {
            oldRec.on(r, host.isHostThread());
        });
        std::this_thread::sleep_for(milliseconds(100));
        HttpRequestConfig fast;
        fast.url = base + "80"; fast.method = "GET"; fast.timeoutMs = 5000;
        http.requestAsyncLatest("status", fast, [&newRec, &host](const NetResult& r) {
            newRec.on(r, host.isHostThread());
        });
        testutil::check(oldRec.waitFor(1, 2000), "poster(B3): superseded(old) callback delivered");
        testutil::check(newRec.waitFor(1, 2000), "poster(B3): new callback delivered");
        testutil::check(oldRec.lastCode == NetErrorCode::Superseded,
                        "poster(B3): old request reported Superseded");
        testutil::check(newRec.lastCode == NetErrorCode::Ok, "poster(B3): new request Ok");
        testutil::check(oldRec.allOnHost && newRec.allOnHost,
                        "poster(B3): both callbacks on host UI thread");
    }

    // ---------- C1：requestAsyncTyped<T> 解析在 worker、结果回调到宿主线程 ----------
    {
        std::atomic<int> parserCalls{0};
        std::atomic<bool> parserOnHost{false};
        TypedRec rec;
        HttpRequestConfig cfg;
        cfg.url = base + "60"; cfg.method = "GET"; cfg.timeoutMs = 5000;
        http.requestAsyncTyped<Demo>(cfg,
            [&](const NetResult& r, Demo& out) {
                ++parserCalls;
                if (host.isHostThread()) parserOnHost.store(true);
                out.code = static_cast<int>(r.code);
                out.body = "BODY:" + r.body;
            },
            [&rec, &host](const Demo& d) { rec.on(d, host.isHostThread()); });
        testutil::check(rec.waitFor(1, 4000), "typed(C1): typed callback delivered");
        testutil::check(rec.count == 1, "typed(C1): exactly one callback");
        testutil::check(rec.allOnHost, "typed(C1): callback runs on host UI thread");
        testutil::check(parserCalls.load() == 1, "typed(C1): parser invoked exactly once");
        testutil::check(!parserOnHost.load(),
                        "typed(C1): parser runs on SDK worker thread, not UI thread");
        testutil::check(rec.code == 0 && rec.body.rfind("BODY:OK:", 0) == 0,
                        "typed(C1): parsed business payload delivered");
    }

    // ---------- C2：超时等错误终态同样会解析+回调（不会卡在「登录中」） ----------
    {
        std::atomic<int> parserCalls{0};
        TypedRec rec;
        HttpRequestConfig cfg;
        cfg.url = base + "900"; cfg.method = "GET"; cfg.timeoutMs = 200;
        http.requestAsyncTyped<Demo>(cfg,
            [&](const NetResult& r, Demo& out) {
                ++parserCalls;
                out.code = static_cast<int>(r.code);
                out.body = r.ok() ? "OK" : ("ERR:" + r.message);
            },
            [&rec, &host](const Demo& d) { rec.on(d, host.isHostThread()); });
        testutil::check(rec.waitFor(1, 2000),
                        "typed(C2): timeout terminal state still delivers callback");
        testutil::check(rec.allOnHost, "typed(C2): timeout callback on host UI thread");
        testutil::check(rec.count == 1 && parserCalls.load() == 1,
                        "typed(C2): exactly one callback + one parse");
        testutil::check(rec.code == static_cast<int>(NetErrorCode::Timeout),
                        "typed(C2): parser saw Timeout error code");
        testutil::check(rec.body.rfind("ERR:", 0) == 0,
                        "typed(C2): business object carries error state");
        std::this_thread::sleep_for(milliseconds(1000));
        testutil::check(rec.count == 1,
                        "typed(C2): late response discarded, no second callback");
    }

    // ---------- 收尾：先清空 poster，避免 shutdown 时向宿主线程投递 ----------
    sdk.setCallbackPoster(CallbackPoster());
    host.waitIdle(500);
    sdk.shutdown();
    std::this_thread::sleep_for(milliseconds(100));
    return testutil::finish("callback_poster");
}
