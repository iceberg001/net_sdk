#include "net_sdk/NetSdk.h"
#include "net_sdk/Logger.h"

namespace net_sdk {

NetSdk& NetSdk::getInstance() {
    static NetSdk instance;
    return instance;
}

NetSdk::NetSdk() {}

NetSdk::~NetSdk() {
    shutdown();
}

bool NetSdk::init(int threadCount, int loopCount) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return initLocked(threadCount, loopCount);
}

void NetSdk::setCallbackPoster(const CallbackPoster& poster) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    callbackPoster_ = poster;  // 值拷贝：DLL 侧持有，避免跨 CRT 堆
    if (!initialized_.load()) return;  // 未 init：保存即可，initLocked() 会应用
    if (httpClient_) httpClient_->setCallbackPoster(poster);
    if (commandRequest_) commandRequest_->setCallbackPoster(poster);
    NET_LOG_INFO("NetSdk", poster
        ? "Host callback poster installed (async callbacks -> host UI thread)"
        : "Host callback poster removed");
}

bool NetSdk::initLocked(int threadCount, int loopCount) {
    if (initialized_.load()) {
        return true;
    }
    // 防御性参数校验：非法线程数/循环数会导致除零或空池。
    if (threadCount < 1) threadCount = 1;
    if (loopCount < 1) loopCount = 1;

    threadPool_ = std::make_unique<ThreadPool>(threadCount);
    eventLoopPool_ = std::make_unique<EventLoopPool>(loopCount);
    eventLoopPool_->start();

    // 统一使用全局单例调度器：消费者通过 getInstance() 访问到的，
    // 与 SDK 内部投递回调用的是同一个实例，队列不再分裂。
    // 若宿主已先行 init(false) 选择了手动 pump，则此处 init() 幂等跳过。
    MainThreadDispatcher& dispatcher = MainThreadDispatcher::getInstance();
    dispatcher.init();

    httpClient_ = std::make_unique<HttpClient>();
    httpClient_->setThreadPool(threadPool_.get());
    httpClient_->setMainThreadDispatcher(&dispatcher);
    httpClient_->setCallbackPoster(callbackPoster_);

    commandRequest_ = std::make_unique<CommandRequest>();
    commandRequest_->setThreadPool(threadPool_.get());
    commandRequest_->setMainThreadDispatcher(&dispatcher);
    commandRequest_->setCallbackPoster(callbackPoster_);

    initialized_.store(true);
    NET_LOG_INFO("NetSdk", "NetSdk initialized: threads=" + std::to_string(threadCount) +
                  " loops=" + std::to_string(loopCount));
    return true;
}

void NetSdk::shutdown() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    shutdownLocked();
}

void NetSdk::shutdownLocked() {
    if (!initialized_.load()) return;
    initialized_.store(false);

    // 关闭顺序至关重要：
    // 1) 先阻止新请求进入；
    // 2) 关闭线程池并等待所有在途异步任务结束（此时回调仍可安全投递到 dispatcher）；
    // 3) 再释放 HTTP / 命令模块（其异步任务已结束，无 use-after-free 风险）；
    // 4) 最后关闭 dispatcher 与事件循环池。
    if (commandRequest_) {
        commandRequest_->cancelAll();
    }

    if (threadPool_) {
        threadPool_->shutdown();
    }

    httpClient_.reset();
    commandRequest_.reset();

    MainThreadDispatcher::getInstance().shutdown();

    if (eventLoopPool_) {
        eventLoopPool_->stop();
    }

    // unique_ptr 在成员析构时会再次调用各对象的 shutdown/stop，
    // 因此这些方法都必须是幂等的（已通过内部标志位保证）。
    eventLoopPool_.reset();
    threadPool_.reset();

    NET_LOG_INFO("NetSdk", "NetSdk shutdown");
}

bool NetSdk::isInitialized() const {
    return initialized_.load();
}

void NetSdk::ensureInitializedLocked() {
    if (!initialized_.load()) {
        initLocked(4, 4);
    }
}

ThreadPool& NetSdk::getThreadPool() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    ensureInitializedLocked();
    return *threadPool_;
}

EventLoopPool& NetSdk::getEventLoopPool() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    ensureInitializedLocked();
    return *eventLoopPool_;
}

HttpClient& NetSdk::getHttpClient() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    ensureInitializedLocked();
    return *httpClient_;
}

CommandRequest& NetSdk::getCommandRequest() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    ensureInitializedLocked();
    return *commandRequest_;
}

MainThreadDispatcher& NetSdk::getMainThreadDispatcher() {
    // 单例永远有效，无需初始化保护，也不受 shutdown 影响。
    return MainThreadDispatcher::getInstance();
}

SignalSlot& NetSdk::getSignalSlot() {
    return SignalSlot::getInstance();
}

void NetSdk::setLogLevel(LogLevel level) {
    Logger::getInstance().setLevel(level);
}

void NetSdk::enableConsoleLog(bool enable) {
    Logger::getInstance().setConsoleOutput(enable);
}

void NetSdk::enableFileLog(bool enable, const std::string& filePath) {
    Logger::getInstance().setFileOutput(enable, filePath);
}

} // namespace net_sdk
