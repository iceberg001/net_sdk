#include "net_sdk/HttpClient.h"
#include "net_sdk/ThreadPool_impl.h"
#include "net_sdk/Logger.h"
#include "net_sdk/MainThreadDispatcher.h"
#include "libhv/http_client.h"
#include "libhv/HttpMessage.h"
#include "libhv/hssl.h"

#include <vector>
#include <unordered_map>
#include <map>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <utility>
#include <filesystem>

namespace net_sdk {

namespace {
void destroyHvClient(void* p) {
    delete static_cast<hv::HttpClient*>(p);
}

NetResult makeErrorResult(NetErrorCode code, const std::string& message) {
    NetResult r;
    r.code = code;
    r.message = message;
    return r;
}
} // anonymous namespace

// ============================================================================
//  HttpImpl：连接池 + 在途请求表 + 超时 watchdog
//
//  生命周期由 shared_ptr 管理：异步任务捕获 impl 而非 this，因此只要还有
//  在途请求，连接槽与 watchdog 就保持存活，彻底避免 use-after-free。
// ============================================================================
struct HttpClient::HttpImpl {
    /// 单个连接槽：一个独立的 hv::HttpClient + 一把只保护自己的锁
    struct Slot {
        void* ctx = nullptr;
        void (*destroy)(void*) = nullptr;
        std::mutex mutex;
        std::string lastScheme;   // 规避 libhv 1.2.4 跨 scheme 复用 keepalive 崩溃

        ~Slot() {
            if (ctx && destroy) destroy(ctx);
            ctx = nullptr;
        }
    };

    /// 单个在途异步请求的状态，deliver() 用 atomic exchange 保证「恰好一次回调」
    struct Pending {
        uint64_t id = 0;
        std::string key;
        NetCallback callback;
        MainThreadDispatcher* dispatcher = nullptr;
        CallbackPoster poster;         // v1.3.0：宿主回调投递器（注册时快照，deliver 无锁）
        bool onWorkerThread = false;   // v1.3.0：工作线程直调（模板方法在此完成解析）
        std::atomic<bool> done{false};
        std::chrono::steady_clock::time_point deadline;

        void deliver(const NetResult& r);
    };

    // ---- 配置：只在初始化/调整时写入，请求路径只读几个标量 ----
    mutable std::mutex configMutex;
    std::atomic<int> concurrency{4};
    int timeoutMs = 30000;
    std::string userAgent = "net_sdk/1.0";
    bool sslVerify = true;
    std::string sslCaFile;
    std::string sslCaPath;
    std::atomic<bool> sslConfigured{false};
    CallbackPoster callbackPoster;   ///< v1.3.0：宿主注入的「投递到主线程」函数

    // ---- 连接槽：懒创建，创建后只读（不再改变 vector 结构） ----
    std::vector<std::unique_ptr<Slot>> slots;
    std::atomic<size_t> nextSlot{0};

    std::atomic<bool> alive{true};

    // ---- 在途请求表 ----
    std::mutex pendingMutex;
    std::unordered_map<uint64_t, std::shared_ptr<Pending>> pendings;
    std::unordered_map<std::string, std::shared_ptr<Pending>> latestByKey;
    std::atomic<uint64_t> nextId{1};

    // ---- 请求级超时 watchdog ----
    std::mutex wdMutex;
    std::condition_variable wdCv;
    bool wdStop = false;
    size_t wdGeneration = 0;   // 注册了更早的 deadline 时唤醒 watchdog 重算
    std::multimap<std::chrono::steady_clock::time_point, std::shared_ptr<Pending>> deadlines;
    std::thread watchdog;

    HttpImpl() {
        watchdog = std::thread([this]() { watchdogLoop(); });
    }

    ~HttpImpl() {
        alive.store(false);
        {
            std::lock_guard<std::mutex> lk(wdMutex);
            wdStop = true;
        }
        wdCv.notify_all();
        if (watchdog.joinable()) {
            // 极端情况下 watchdog 自己触发了最后一个 impl 引用的释放，
            // 不能 join 自身，改为 detach 让线程自然退出。
            if (watchdog.get_id() == std::this_thread::get_id()) {
                watchdog.detach();
            } else {
                watchdog.join();
            }
        }
    }

    /// HttpClient 析构时调用：阻止新请求、停 watchdog、给未回调的请求一个终态。
    /// 注意这里不销毁连接槽——由 impl 的最后一个 shared_ptr 负责。
    void beginShutdown() {
        alive.store(false);
        {
            std::lock_guard<std::mutex> lk(wdMutex);
            wdStop = true;
            ++wdGeneration;
        }
        wdCv.notify_all();

        std::vector<std::shared_ptr<Pending>> rest;
        {
            std::lock_guard<std::mutex> lk(pendingMutex);
            for (auto& kv : pendings) rest.push_back(kv.second);
            pendings.clear();
            latestByKey.clear();
        }
        for (auto& p : rest) {
            p->deliver(makeErrorResult(NetErrorCode::ConnectionClosed, "HttpClient destroyed"));
        }
    }

    /// 读取宿主投递器（值拷贝，避免 deliver 时再访问 impl 状态）
    CallbackPoster getCallbackPoster() const {
        std::lock_guard<std::mutex> lk(configMutex);
        return callbackPoster;
    }

    void ensureSlots() {
        std::lock_guard<std::mutex> lk(configMutex);
        if (!slots.empty()) return;        const int n = concurrency.load();
        for (int i = 0; i < n; ++i) {
            auto s = std::make_unique<Slot>();
            s->ctx = new hv::HttpClient();
            s->destroy = &destroyHvClient;
            slots.push_back(std::move(s));
        }
    }

    void registerDeadline(const std::shared_ptr<Pending>& p) {
        {
            std::lock_guard<std::mutex> lk(wdMutex);
            deadlines.emplace(p->deadline, p);
            ++wdGeneration;
        }
        wdCv.notify_all();
    }

    /// 请求结束（成功/超时/被取代/取消）后清理在途表，幂等
    void finish(const std::shared_ptr<Pending>& p) {
        std::lock_guard<std::mutex> lk(pendingMutex);
        pendings.erase(p->id);
        if (!p->key.empty()) {
            auto it = latestByKey.find(p->key);
            if (it != latestByKey.end() && it->second == p) {
                latestByKey.erase(it);
            }
        }
    }

    void watchdogLoop() {
        std::unique_lock<std::mutex> lk(wdMutex);
        size_t seenGen = wdGeneration;
        for (;;) {
            if (wdStop) return;
            if (deadlines.empty()) {
                wdCv.wait(lk, [this, &seenGen]() {
                    return wdStop || wdGeneration != seenGen;
                });
                if (wdStop) return;
                seenGen = wdGeneration;
                continue;
            }

            const auto next = deadlines.begin()->first;
            // 返回 true 表示 wdStop 或注册了新 deadline（需重算最近到期时间），
            // 返回 false 表示等待到时（可能仍有 spurious wake，按 now 逐条判断）。
            const bool changed = wdCv.wait_until(lk, next, [this, &seenGen]() {
                return wdStop || wdGeneration != seenGen;
            });
            if (wdStop) return;
            if (changed) {
                seenGen = wdGeneration;
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            std::vector<std::shared_ptr<Pending>> expired;
            for (auto it = deadlines.begin(); it != deadlines.end(); ) {
                if (it->first <= now) {
                    expired.push_back(it->second);
                    it = deadlines.erase(it);
                } else {
                    break;
                }
            }
            if (!expired.empty()) {
                lk.unlock();
                for (auto& p : expired) {
                    finish(p);
                    p->deliver(makeErrorResult(NetErrorCode::Timeout,
                                               "Request timed out (SDK watchdog)"));
                }
                lk.lock();
            }
        }
    }
};

void HttpClient::HttpImpl::Pending::deliver(const NetResult& r) {
    // 成功/超时/被取代/取消/析构五条路径可能并发到达，exchange 保证只回调一次。
    if (done.exchange(true)) return;
    if (!callback) return;
    NetResult copy = r;  // 显式拷贝，避免闭包与临时对象生命周期耦合
    auto cb = callback;
    std::function<void()> task = [cb, copy]() { cb(copy); };

    // v1.3.0 线程模型（优先级从高到低）：
    // 1) 模板方法：handler 在工作线程直调，解析完成后自行把结果投递到主线程；
    // 2) 宿主注入的 poster：所有结果回调切到宿主 UI 主线程（修复回调丢失的关键）；
    // 3) 旧行为：dispatcher 泵线程；
    // 4) 都没有：就地执行（工作线程）。
    if (onWorkerThread) { task(); return; }
    if (poster) { poster(std::move(task)); return; }
    if (dispatcher) { dispatcher->post(task); return; }
    task();
}

HttpClient::HttpClient()
    : impl_(std::make_shared<HttpImpl>()) {
    initialized_.store(true);
    NET_LOG_DEBUG("HttpClient", "HttpClient initialized (connection slots=" +
                  std::to_string(impl_->concurrency.load()) + ")");
}

HttpClient::~HttpClient() {
    impl_->beginShutdown();
}

void HttpClient::setThreadPool(ThreadPool* pool) {
    threadPool_ = pool;
}

void HttpClient::setMainThreadDispatcher(MainThreadDispatcher* dispatcher) {
    dispatcher_ = dispatcher;
}

void HttpClient::setCallbackPoster(const CallbackPoster& poster) {
    // DLL 侧值拷贝：宿主传入的 std::function 在本模块堆上分配/释放，避免跨 CRT 堆。
    std::lock_guard<std::mutex> lock(impl_->configMutex);
    impl_->callbackPoster = poster;
    NET_LOG_INFO("HttpClient", poster
        ? "Host callback poster installed: async callbacks will run on host thread"
        : "Host callback poster removed: falling back to dispatcher");
}

CallbackPoster HttpClient::makePosterFn() const {
    CallbackPoster poster;
    {
        std::lock_guard<std::mutex> lock(impl_->configMutex);
        poster = impl_->callbackPoster;
    }
    MainThreadDispatcher* d = dispatcher_;
    if (poster) {
        // 只值捕获 poster，不捕获 this：即使 HttpClient 先析构，在途请求仍安全。
        return [poster](std::function<void()> task) { poster(std::move(task)); };
    }
    if (d) {
        return [d](std::function<void()> task) { d->post(task); };
    }
    return CallbackPoster();  // 空：调用方就地执行
}

void HttpClient::postToMain(const std::function<void()>& task) {
    auto post = makePosterFn();
    if (post) post(task);
    else task();
}

void HttpClient::setTimeout(int timeoutMs) {
    std::lock_guard<std::mutex> lock(impl_->configMutex);
    impl_->timeoutMs = timeoutMs;
}

void HttpClient::setUserAgent(const std::string& userAgent) {
    std::lock_guard<std::mutex> lock(impl_->configMutex);
    impl_->userAgent = userAgent;
}

void HttpClient::setConcurrency(int concurrency) {
    if (concurrency < 1) concurrency = 1;
    std::lock_guard<std::mutex> lock(impl_->configMutex);
    if (!impl_->slots.empty()) {
        NET_LOG_WARN("HttpClient",
                     "setConcurrency ignored: connection pool already created; "
                     "call it before the first request");
        return;
    }
    impl_->concurrency.store(concurrency);
}

int HttpClient::getConcurrency() const {
    return impl_->concurrency.load();
}

void HttpClient::configureSsl(bool verify, const std::string& caFile, const std::string& caPath) {
    {
        std::lock_guard<std::mutex> lock(impl_->configMutex);
        impl_->sslVerify = verify;
        impl_->sslCaFile = caFile;
        impl_->sslCaPath = caPath;
        impl_->sslConfigured.store(true);
    }
    if (!(HV_WITH_SSL)) {
        NET_LOG_WARN("HttpClient", "SSL settings saved but inactive: linked libhv has no SSL backend");
        return;
    }
#ifdef NET_SDK_HAS_SSL
    // hssl_ctx_init 会把新 ctx 赋给全局 g_ssl_ctx，http_client 连接时通过
    // hssl_ctx_instance() 取到它，因此 verify/caFile/caPath 会真正生效。
    hssl_ctx_init_param_t param{};
    param.endpoint = HSSL_CLIENT;
    param.verify_peer = verify ? 1 : 0;
    if (!caFile.empty()) param.ca_file = caFile.c_str();
    if (!caPath.empty()) param.ca_path = caPath.c_str();
    hssl_ctx_t ctx = hssl_ctx_init(&param);
    if (!ctx) {
        NET_LOG_ERROR("HttpClient", "Failed to initialize SSL context");
        return;
    }
    NET_LOG_INFO("HttpClient", std::string("SSL configured: verify=") + (verify ? "true" : "false") +
                  " caFile=" + (caFile.empty() ? "<system>" : caFile));
#else
    (void)verify; (void)caFile; (void)caPath;
#endif
}

NetResult HttpClient::requestImpl(HttpImpl& impl, const HttpRequestConfig& config) {
    if (!impl.alive.load()) {
        return makeErrorResult(NetErrorCode::ConnectionClosed, "HttpClient not available");
    }

    // 懒创建连接槽（幂等）。同步与异步请求都走这里。
    impl.ensureSlots();

    int defaultTimeout = 30000;
    std::string userAgent;
    size_t slotCount = 0;
    {
        std::lock_guard<std::mutex> lock(impl.configMutex);
        defaultTimeout = impl.timeoutMs;
        userAgent = impl.userAgent;
        slotCount = impl.slots.size();
    }
    if (slotCount == 0) {
        return makeErrorResult(NetErrorCode::UnknownError, "No HTTP connection slot available");
    }

    const bool isHttps = config.url.rfind("https://", 0) == 0;
    if (isHttps && !(HV_WITH_SSL)) {
        NET_LOG_ERROR("HttpClient", "HTTPS not supported: linked libhv has no SSL backend");
        return makeErrorResult(NetErrorCode::UnknownError,
                               "HTTPS not supported: linked libhv has no SSL backend");
    }

    // 选槽：优先 try_lock 抢占空闲槽，实现真正并发；全部忙碌时才排队等其中一个，
    // 从而把并发量限制在 setConcurrency 设定值，避免无限建连。
    HttpImpl::Slot* slot = nullptr;
    std::unique_lock<std::mutex> slotLock;
    for (size_t k = 0; k < slotCount; ++k) {
        const size_t idx = impl.nextSlot.fetch_add(1) % slotCount;
        HttpImpl::Slot* candidate = impl.slots[idx].get();
        std::unique_lock<std::mutex> tmp(candidate->mutex, std::try_to_lock);
        if (tmp.owns_lock()) {
            slot = candidate;
            slotLock = std::move(tmp);
            break;
        }
    }
    if (!slot) {
        const size_t idx = impl.nextSlot.fetch_add(1) % slotCount;
        slot = impl.slots[idx].get();
        slotLock = std::unique_lock<std::mutex>(slot->mutex);  // 阻塞等待空闲槽
    }

    if (!slot->ctx || !impl.alive.load()) {
        return makeErrorResult(NetErrorCode::ConnectionClosed, "HttpClient not available");
    }
    auto* client = static_cast<hv::HttpClient*>(slot->ctx);

    // 规避 libhv 1.2.4 的跨 scheme keepalive 复用缺陷（每个槽独立记录）。
    const std::string scheme = isHttps ? "https" : "http";
    if (!slot->lastScheme.empty() && slot->lastScheme != scheme) {
        client->close();
    }
    slot->lastScheme = scheme;

    HttpRequest req;
    req.method = http_method_enum(config.method.c_str());
    req.url = config.url;
    // libhv 的 timeout 以秒计；向上取整避免 <1000ms 的超时被整除成 0（0 表示无限等待）。
    const int timeoutMs = config.timeoutMs > 0 ? config.timeoutMs : defaultTimeout;
    req.timeout = timeoutMs > 0 ? (timeoutMs + 999) / 1000 : 0;
    req.redirect = config.followRedirect ? 1 : 0;

    for (const auto& [key, value] : config.headers) {
        req.headers[key] = value;
    }
    if (req.headers.find("User-Agent") == req.headers.end()) {
        req.headers["User-Agent"] = userAgent;
    }
    if (!config.body.empty()) {
        req.body = config.body;
    }

    HttpResponse resp;
    const int ret = client->send(&req, &resp);
    if (ret != 0) {
        NET_LOG_ERROR("HttpClient", "Request failed: " + std::to_string(ret));
        return makeErrorResult(NetErrorCode::ConnectFailed,
                               "Request failed: " + std::to_string(ret));
    }

    NetResult result;
    result.statusCode = resp.status_code;
    result.body = resp.body;
    result.code = NetErrorCode::Ok;
    for (const auto& [key, value] : resp.headers) {
        result.headers[key] = value;
    }
    if (resp.status_code >= 400) {
        result.code = NetErrorCode::UnknownError;
        result.message = "HTTP error: " + std::to_string(resp.status_code);
    }

    NET_LOG_DEBUG("HttpClient", "Request completed: " + config.url +
                  " status=" + std::to_string(resp.status_code));
    return result;
}

NetResult HttpClient::doRequestSync(const HttpRequestConfig& config) {
    return requestImpl(*impl_, config);
}

NetResult HttpClient::requestSync(const HttpRequestConfig& config) {
    return doRequestSync(config);
}

uint64_t HttpClient::doRequestAsync(const HttpRequestConfig& config, NetCallback callback,
                                    const std::string& key, bool callbackOnWorker) {
    auto impl = impl_;
    if (!impl->alive.load()) {
        auto r = makeErrorResult(NetErrorCode::ConnectionClosed, "HttpClient not available");
        if (callback) {
            auto post = makePosterFn();
            std::function<void()> task = [cb = std::move(callback), r]() { cb(r); };
            if (post) post(std::move(task));
            else task();
        }
        return 0;
    }
    impl->ensureSlots();

    int defaultTimeout = 30000;
    {
        std::lock_guard<std::mutex> lock(impl->configMutex);
        defaultTimeout = impl->timeoutMs;
    }
    const int timeoutMs = config.timeoutMs > 0 ? config.timeoutMs : defaultTimeout;

    auto pending = std::make_shared<HttpImpl::Pending>();
    pending->id = impl->nextId.fetch_add(1);
    pending->key = key;
    pending->callback = std::move(callback);
    pending->dispatcher = dispatcher_;
    pending->poster = impl->getCallbackPoster();
    pending->onWorkerThread = callbackOnWorker;
    pending->deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);

    // 同 key 覆盖：取出旧请求（锁外再回调，避免用户回调重入死锁）。
    std::shared_ptr<HttpImpl::Pending> superseded;
    {
        std::lock_guard<std::mutex> lock(impl->pendingMutex);
        impl->pendings[pending->id] = pending;
        if (!key.empty()) {
            auto it = impl->latestByKey.find(key);
            if (it != impl->latestByKey.end()) {
                superseded = it->second;
            }
            impl->latestByKey[key] = pending;
        }
    }
    impl->registerDeadline(pending);

    if (superseded) {
        superseded->deliver(makeErrorResult(NetErrorCode::Superseded,
                                            "Superseded by a newer request with the same key"));
        impl->finish(superseded);
    }

    // 捕获 impl（shared_ptr）而非 this：HttpClient 析构后在途任务仍安全。
    auto task = [impl, pending, config]() {
        NetResult result = requestImpl(*impl, config);
        impl->finish(pending);
        pending->deliver(result);  // 若已超时/被取代，这里自动跳过
    };

    if (threadPool_) {
        threadPool_->submitVoid(task);
    } else {
        task();
    }
    return pending->id;
}

uint64_t HttpClient::requestAsync(const HttpRequestConfig& config, NetCallback callback) {
    return doRequestAsync(config, std::move(callback), std::string());
}

uint64_t HttpClient::requestAsyncLatest(const std::string& key,
                                        const HttpRequestConfig& config,
                                        NetCallback callback) {
    return doRequestAsync(config, std::move(callback), key);
}

bool HttpClient::cancel(uint64_t requestId) {
    std::shared_ptr<HttpImpl::Pending> p;
    {
        std::lock_guard<std::mutex> lock(impl_->pendingMutex);
        auto it = impl_->pendings.find(requestId);
        if (it == impl_->pendings.end()) return false;
        p = it->second;
    }
    impl_->finish(p);
    p->deliver(makeErrorResult(NetErrorCode::Canceled, "Request canceled"));
    return true;
}

void HttpClient::cancelAll() {
    std::vector<std::shared_ptr<HttpImpl::Pending>> rest;
    {
        std::lock_guard<std::mutex> lock(impl_->pendingMutex);
        for (auto& kv : impl_->pendings) rest.push_back(kv.second);
        impl_->pendings.clear();
        impl_->latestByKey.clear();
    }
    for (auto& p : rest) {
        p->deliver(makeErrorResult(NetErrorCode::Canceled, "Request canceled"));
    }
}

NetResult HttpClient::getSync(const std::string& url) {
    HttpRequestConfig config;
    config.url = url;
    config.method = "GET";
    return requestSync(config);
}

NetResult HttpClient::postSync(const std::string& url, const std::string& body, const std::string& contentType) {
    HttpRequestConfig config;
    config.url = url;
    config.method = "POST";
    config.body = body;
    config.headers["Content-Type"] = contentType;
    return requestSync(config);
}

NetResult HttpClient::putSync(const std::string& url, const std::string& body, const std::string& contentType) {
    HttpRequestConfig config;
    config.url = url;
    config.method = "PUT";
    config.body = body;
    config.headers["Content-Type"] = contentType;
    return requestSync(config);
}

NetResult HttpClient::deleteSync(const std::string& url) {
    HttpRequestConfig config;
    config.url = url;
    config.method = "DELETE";
    return requestSync(config);
}

void HttpClient::getAsync(const std::string& url, NetCallback callback) {
    HttpRequestConfig config;
    config.url = url;
    config.method = "GET";
    requestAsync(config, std::move(callback));
}

void HttpClient::postAsync(const std::string& url, const std::string& body, NetCallback callback, const std::string& contentType) {
    HttpRequestConfig config;
    config.url = url;
    config.method = "POST";
    config.body = body;
    config.headers["Content-Type"] = contentType;
    requestAsync(config, std::move(callback));
}

void HttpClient::putAsync(const std::string& url, const std::string& body, NetCallback callback, const std::string& contentType) {
    HttpRequestConfig config;
    config.url = url;
    config.method = "PUT";
    config.body = body;
    config.headers["Content-Type"] = contentType;
    requestAsync(config, std::move(callback));
}

void HttpClient::deleteAsync(const std::string& url, NetCallback callback) {
    HttpRequestConfig config;
    config.url = url;
    config.method = "DELETE";
    requestAsync(config, std::move(callback));
}

void HttpClient::downloadFile(const std::string& url, const std::string& savePath, NetCallback callback) {
    auto impl = impl_;
    auto post = makePosterFn();
    impl->ensureSlots();
    auto task = [impl, post, url, savePath, callback]() {
        HttpRequestConfig config;
        config.url = url;
        config.method = "GET";
        NetResult result = requestImpl(*impl, config);
        if (result.ok() && !result.body.empty()) {
            FILE* fp = fopen(savePath.c_str(), "wb");
            if (fp) {
                fwrite(result.body.data(), 1, result.body.size(), fp);
                fclose(fp);
                result.message = "Downloaded to " + savePath;
                NET_LOG_INFO("HttpClient", result.message);
            } else {
                result.code = NetErrorCode::UnknownError;
                result.message = "Failed to save file: " + savePath;
                NET_LOG_ERROR("HttpClient", result.message);
            }
        }
        if (!callback) return;
        std::function<void()> deliver = [callback, result]() { callback(result); };
        if (post) post(std::move(deliver));
        else deliver();
    };

    if (threadPool_) {
        threadPool_->submitVoid(task);
    } else {
        task();
    }
}

uint64_t HttpClient::downloadAsync(const DownloadConfig& config,
                                   DownloadCallback callback,
                                   DownloadProgressCallback progress) {
    auto impl = impl_;
    auto post = makePosterFn();

    namespace fs = std::filesystem;

    std::string filename = config.filename;
    if (filename.empty()) {
        std::string urlPath = config.url;
        size_t qpos = urlPath.find('?');
        if (qpos != std::string::npos) urlPath = urlPath.substr(0, qpos);
        size_t spos = urlPath.rfind('/');
        if (spos != std::string::npos && spos + 1 < urlPath.size()) {
            filename = urlPath.substr(spos + 1);
        }
        if (filename.empty()) filename = "download";
    }

    fs::path savePath = fs::path(config.downloadDir) / filename;
    std::string savePathStr = savePath.string();

    std::error_code ec;
    fs::create_directories(config.downloadDir, ec);

    if (!config.overwrite && fs::exists(savePath, ec)) {
        DownloadResult result;
        result.code = NetErrorCode::InvalidParam;
        result.message = "File already exists: " + savePathStr;
        result.url = config.url;
        result.filePath = savePathStr;
        if (callback) {
            std::function<void()> task = [callback, result]() { callback(result); };
            if (post) post(std::move(task));
            else task();
        }
        return 0;
    }

    uint64_t downloadId = impl->nextId.fetch_add(1);

    impl->ensureSlots();

    auto task = [impl, post, config, savePathStr, callback, progress, downloadId]() {
        HttpRequestConfig reqConfig;
        reqConfig.url = config.url;
        reqConfig.method = "GET";
        reqConfig.headers = config.headers;
        if (config.timeoutMs > 0) reqConfig.timeoutMs = config.timeoutMs;

        NetResult netResult = requestImpl(*impl, reqConfig);

        DownloadResult dlResult;
        dlResult.url = config.url;
        dlResult.filePath = savePathStr;

        if (!netResult.ok()) {
            dlResult.code = netResult.code;
            dlResult.message = netResult.message;
        } else if (netResult.body.empty()) {
            dlResult.code = NetErrorCode::UnknownError;
            dlResult.message = "Downloaded content is empty";
        } else {
            dlResult.totalBytes = static_cast<int64_t>(netResult.body.size());

            if (progress) {
                auto prog = progress;
                int64_t total = dlResult.totalBytes;
                std::function<void()> progTask = [prog, total]() { prog(0, total); };
                if (post) post(std::move(progTask));
                else progTask();
            }

            FILE* fp = fopen(savePathStr.c_str(), "wb");
            if (fp) {
                const size_t chunkSize = 65536;
                const size_t totalSize = netResult.body.size();
                size_t written = 0;
                auto lastReport = std::chrono::steady_clock::now();

                while (written < totalSize) {
                    size_t toWrite = (std::min)(chunkSize, totalSize - written);
                    fwrite(netResult.body.data() + written, 1, toWrite, fp);
                    written += toWrite;

                    auto now = std::chrono::steady_clock::now();
                    if (progress && now - lastReport >= std::chrono::seconds(1)) {
                        auto prog = progress;
                        int64_t downloaded = static_cast<int64_t>(written);
                        int64_t total = static_cast<int64_t>(totalSize);
                        std::function<void()> progTask = [prog, downloaded, total]() {
                            prog(downloaded, total);
                        };
                        if (post) post(std::move(progTask));
                        else progTask();
                        lastReport = now;
                    }
                }

                fclose(fp);
                dlResult.downloadedBytes = static_cast<int64_t>(totalSize);
                dlResult.message = "Downloaded to " + savePathStr;
                NET_LOG_INFO("HttpClient", dlResult.message);

                if (progress) {
                    auto prog = progress;
                    int64_t downloaded = static_cast<int64_t>(totalSize);
                    int64_t total = static_cast<int64_t>(totalSize);
                    std::function<void()> progTask = [prog, downloaded, total]() {
                        prog(downloaded, total);
                    };
                    if (post) post(std::move(progTask));
                    else progTask();
                }
            } else {
                dlResult.code = NetErrorCode::UnknownError;
                dlResult.message = "Failed to save file: " + savePathStr;
                NET_LOG_ERROR("HttpClient", dlResult.message);
            }
        }

        if (callback) {
            std::function<void()> task = [callback, dlResult]() { callback(dlResult); };
            if (post) post(std::move(task));
            else task();
        }
    };

    if (threadPool_) {
        threadPool_->submitVoid(task);
    } else {
        task();
    }

    return downloadId;
}

} // namespace net_sdk
