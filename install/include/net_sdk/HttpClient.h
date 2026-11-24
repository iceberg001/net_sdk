/**
 * @file HttpClient.h
 * @brief NetSDK HTTP 客户端
 *
 * 基于 libhv 的 hv::HttpClient 封装，提供同步和异步 HTTP/HTTPS 请求能力。
 * 支持 GET/POST/PUT/DELETE、自定义头部、超时控制、文件下载等。
 *
 * 并发模型（v1.1 起）：
 *  - 内部为「多连接槽」结构：默认 4 个互相独立的 hv::HttpClient，
 *    每个槽一把锁。请求优先占用空闲槽，全部忙碌时才排队等待其中一个槽，
 *    因此不同请求可真正并发，单个慢请求不会阻塞其它请求（无全局队头阻塞）。
 *  - 每个异步请求自带 SDK 级 watchdog：到 config.timeoutMs 未返回即回调
 *    NetErrorCode::Timeout；迟到的网络响应被丢弃，保证「每请求恰好一次回调」。
 *  - requestAsyncLatest(key, ...) 提供同 key 覆盖语义：新请求到来时旧请求
 *    立即以 NetErrorCode::Superseded 回调，新请求立即发出、不等待旧请求。
 *
 * 线程模型（v1.3.0 起，重要）：
 *  - 网络 IO 与「模板解析」都在 SDK 内部线程池执行，不阻塞宿主 UI 线程；
 *  - 回调默认经 MainThreadDispatcher 投递：其 autoPump=true（默认）时任务
 *    跑在 SDK 内部泵线程，**并不等于宿主 UI 主线程**；
 *  - 宿主调用 setCallbackPoster() 注入「投递到 UI 主线程」的函数后，所有
 *    异步结果回调（含 requestAsyncTyped 的强类型结果）都会在该线程执行，
 *    业务层无需、也不应再做二次线程切换（这正是历史上回调丢失的根因）。
 *  - 未注入 poster 时保持旧行为（dispatcher / 直接调用），向后兼容。
 *
 * 典型用法（宿主 UI 应用，一行注入 + 一行请求）：
 * @code
 *   // 1) 进程启动时注入一次：告诉 SDK 如何把回调投递到你的 UI 主线程
 *   net_sdk::NetSdk::getInstance().setCallbackPoster([](std::function<void()> t) {
 *       PostToMyUiThread(std::move(t));   // 线程安全，且保证任务一定被执行
 *   });
 *
 *   // 2) 上层只传参数 + 一个解析函数 + 强类型回调：
 *   //    异步 / 超时 / 覆盖 / 解析 / 切主线程全部由 SDK 底层完成
 *   net_sdk::HttpRequestConfig cfg; cfg.url = url; cfg.method = "POST";
 *   cfg.body = json; cfg.timeoutMs = 10000;
 *   http.requestAsyncTyped<LoginRsp>(cfg,
 *       [](const net_sdk::NetResult& r, LoginRsp& out) { ParseInto(r, out); },
 *       [](const LoginRsp& rsp) { OnLoginRsp(rsp); },   // 已在 UI 主线程
 *       "loginEmployeeAccount");                          // key 非空：同 key 覆盖
 * @endcode
 *
 * @author NetSDK Team
 * @version 1.3.0
 * @date 2026-11-19
 */

#pragma once

#include "Common.h"
#include "ThreadPool.h"
#include <memory>
#include <atomic>
#include <mutex>
#include <string>
#include <cstdint>
#include <utility>

namespace net_sdk {

class MainThreadDispatcher;

/**
 * @brief HTTP 客户端
 *
 * 封装多个 hv::HttpClient 连接槽，提供同步和异步请求接口。
 * 异步请求通过 ThreadPool 在工作线程执行；回调线程由 setCallbackPoster()
 * 决定，未注入时回退到 MainThreadDispatcher（见文件头「线程模型」）。
 */
class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    /// 设置线程池（用于异步请求）
    void setThreadPool(ThreadPool* pool);

    /// 设置主线程调度器（未注入 poster 时的回退路径）
    void setMainThreadDispatcher(MainThreadDispatcher* dispatcher);

    /**
     * @brief 设置宿主回调投递器（v1.3.0，回调到主线程的关键）
     *
     * 注入后，所有异步结果回调都会通过它投递到宿主指定线程（通常是 UI 主线程）。
     * 业务层不再需要自己 PostTask / 切线程。传空函数可取消注入。
     * 详见 Common.h 中 CallbackPoster 的契约说明。
     */
    void setCallbackPoster(const CallbackPoster& poster);

    /**
     * @brief 把一个任务投递到宿主主线程（线程安全）
     *
     * 优先走已注入的 poster，其次回退 dispatcher，都没有时就地执行。
     * 供宿主业务代码在自定义场景复用；SDK 内部回调已自动使用它。
     */
    void postToMain(const std::function<void()>& task);

    /// 设置默认超时时间（毫秒）
    void setTimeout(int timeoutMs);

    /// 设置 User-Agent
    void setUserAgent(const std::string& userAgent);

    /**
     * @brief 设置最大并发连接数（默认 4）
     *
     * 决定内部连接槽数量：N 个请求可同时占用 N 个槽真正并发，
     * 第 N+1 个请求起排队等待空闲槽。请在首次发起请求之前调用
     * （连接槽为懒创建，创建后调整会被忽略并输出一条警告）。
     */
    void setConcurrency(int concurrency);

    /// 获取当前并发连接上限
    int getConcurrency() const;

    /**
     * @brief 配置 SSL/TLS
     *
     * 随 SDK 发布的 libhv 已启用 OpenSSL，HTTPS 开箱可用：
     * 未调用本方法时，首次 HTTPS 请求会自动创建一个不校验对端证书的 SSL_CTX。
     * 若需校验证书（生产环境推荐），请在首次 HTTPS 请求之前调用本方法，
     * verify=true 时启用对端证书校验，可用 caFile/caPath 指定自签 CA。
     *
     * 注意：底层 SSL_CTX 是进程级全局对象，多次调用以最后一次为准。
     */
    void configureSsl(bool verify, const std::string& caFile = "", const std::string& caPath = "");

    /// 同步 HTTP 请求
    NetResult requestSync(const HttpRequestConfig& config);

    /**
     * @brief 异步 HTTP 请求
     *
     * 请求在独立连接槽上执行，与其它请求并发；到 config.timeoutMs 未返回
     * 会以 NetErrorCode::Timeout 回调一次。回调线程见文件头「线程模型」：
     * 注入 poster 后即为宿主主线程。
     *
     * @return 请求 ID，可传给 cancel() 取消
     */
    uint64_t requestAsync(const HttpRequestConfig& config, NetCallback callback);

    /**
     * @brief 同 key「只保留最新」的异步请求
     *
     * 相同 key 的新请求到来时，旧请求立即以 NetErrorCode::Superseded 回调一次；
     * 新请求立即发出、不排队等待旧请求；旧请求迟到的网络响应被丢弃。
     */
    uint64_t requestAsyncLatest(const std::string& key, const HttpRequestConfig& config, NetCallback callback);

    /**
     * @brief 【推荐】一体化异步请求：底层完成异步 + 模板解析 + 回调到主线程
     *
     * 上层只需传请求参数、一个「网络结果 → 业务结构体」的解析函数、以及一个
     * 强类型回调。SDK 保证：
     *  - 网络 IO 与 parser 都在内部工作线程执行（不阻塞 UI）；
     *  - 成功 / 超时 / 被取代 / 取消 / 连接关闭等**所有终态**都恰好回调一次，
     *    绝不静默丢弃（杜绝「卡在登录中」这类问题）；
     *  - parser 抛异常时仍会以默认构造的 T 回调一次，不会吞掉回调；
     *  - 回调通过 setCallbackPoster() 注入的投递器切到宿主主线程。
     *
     * parser 契约：必须处理所有 NetResult（含 !result.ok() 的错误码），把错误
     * 状态写入 out；SDK 不认识业务结构体，故解析规则由业务提供（可复用 x2struct
     * 或 nlohmann 的 get<T>()）。
     *
     * @tparam T 业务响应结构体（需可默认构造、可拷贝）
     * @param config 请求配置
     * @param parser 解析函数，签名 void(const NetResult&, T&)，在工作线程执行
     * @param callback 强类型回调，签名 void(const T&)，在宿主主线程执行
     * @param key 非空时启用同 key 覆盖语义（等价 requestAsyncLatest）
     * @return 请求 ID
     */
    template<typename T>
    uint64_t requestAsyncTyped(const HttpRequestConfig& config,
                               const std::function<void(const NetResult&, T&)>& parser,
                               const std::function<void(const T&)>& callback,
                               const std::string& key = "") {
        // 全部显式拷贝为值，闭包不捕获 this / 不依赖 HttpClient 生命周期。
        std::function<void(const NetResult&, T&)> p = parser;
        std::function<void(const T&)> cb = callback;
        CallbackPoster post = makePosterFn();

        NetCallback handler = [p, cb, post](const NetResult& r) {
            T value{};
            if (p) {
                try { p(r, value); }
                catch (...) { /* 仍以默认 value 回调一次，绝不丢回调 */ }
            }
            if (!cb) return;
            std::function<void()> task = [cb, value]() { cb(value); };
            if (post) post(std::move(task));
            else task();
        };
        // callbackOnWorker=true：handler 在工作线程跑（解析在此完成），
        // 主线程回调由 handler 内部的 post 投递，只有一次线程切换。
        return doRequestAsync(config, std::move(handler), key, true);
    }

    /**
     * @brief 取消一个在途异步请求
     * @note 被取消的请求会以 NetErrorCode::Canceled 回调一次
     */
    bool cancel(uint64_t requestId);

    /// 取消全部在途异步请求（shutdown 时自动调用）
    void cancelAll();

    /// 同步 GET 请求
    NetResult getSync(const std::string& url);

    /// 同步 POST 请求
    NetResult postSync(const std::string& url, const std::string& body, const std::string& contentType = "application/json");

    /// 同步 PUT 请求
    NetResult putSync(const std::string& url, const std::string& body, const std::string& contentType = "application/json");

    /// 同步 DELETE 请求
    NetResult deleteSync(const std::string& url);

    /// 异步 GET 请求
    void getAsync(const std::string& url, NetCallback callback);

    /// 异步 POST 请求
    void postAsync(const std::string& url, const std::string& body, NetCallback callback, const std::string& contentType = "application/json");

    /// 异步 PUT 请求
    void putAsync(const std::string& url, const std::string& body, NetCallback callback, const std::string& contentType = "application/json");

    /// 异步 DELETE 请求
    void deleteAsync(const std::string& url, NetCallback callback);

    /// 下载文件
    void downloadFile(const std::string& url, const std::string& savePath, NetCallback callback);

private:
    // 异步任务持有 shared_ptr<HttpImpl> 而非捕获 this，因此即使 HttpClient
    // 被析构，在途任务与连接槽也能安全存活到全部完成，杜绝 use-after-free。
    struct HttpImpl;

    static NetResult requestImpl(HttpImpl& impl, const HttpRequestConfig& config);

    NetResult doRequestSync(const HttpRequestConfig& config);
    // callbackOnWorker=true 时 handler 在工作线程直接执行（供模板方法做解析），
    // false（默认）时经 poster / dispatcher 投递回调。
    uint64_t doRequestAsync(const HttpRequestConfig& config, NetCallback callback,
                            const std::string& key, bool callbackOnWorker = false);

    /// 生成「把任务投递到宿主主线程」的函数（值捕获，不依赖 this 生命周期）
    CallbackPoster makePosterFn() const;

    std::shared_ptr<HttpImpl> impl_;
    ThreadPool* threadPool_ = nullptr;
    MainThreadDispatcher* dispatcher_ = nullptr;
    std::atomic<bool> initialized_{false};
};

} // namespace net_sdk
