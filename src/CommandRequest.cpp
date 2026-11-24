#include "net_sdk/CommandRequest.h"
#include "net_sdk/ThreadPool.h"
#include "net_sdk/MainThreadDispatcher.h"
#include "net_sdk/Logger.h"

#include <chrono>
#include <random>
#include <sstream>

namespace net_sdk {

CommandRequest::CommandRequest() {
    // 启动超时检测线程：否则 checkTimeout() 永远不会被调用，
    // 无人应答的请求将永久挂起并泄漏。
    timeoutRunning_.store(true);
    timeoutThread_ = std::make_unique<std::thread>([this]() { timeoutLoop(); });
    NET_LOG_DEBUG("CommandRequest", "CommandRequest initialized");
}

CommandRequest::~CommandRequest() {
    timeoutRunning_.store(false);
    if (timeoutThread_ && timeoutThread_->joinable()) {
        timeoutThread_->join();
    }
    cancelAll();
}

void CommandRequest::timeoutLoop() {
    while (timeoutRunning_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!timeoutRunning_.load()) break;
        checkTimeout();
    }
}

void CommandRequest::setThreadPool(ThreadPool* pool) {
    threadPool_ = pool;
}

void CommandRequest::setMainThreadDispatcher(MainThreadDispatcher* dispatcher) {
    mainThreadDispatcher_ = dispatcher;
}

void CommandRequest::setCallbackPoster(const CallbackPoster& poster) {
    std::lock_guard<std::mutex> lock(posterMutex_);
    callbackPoster_ = poster;  // 值拷贝，DLL 侧持有
}

std::string CommandRequest::generateRequestId() {
    int counter = requestCounter_.fetch_add(1);
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    std::ostringstream oss;
    oss << "req_" << ms << "_" << counter;
    return oss.str();
}

void CommandRequest::send(const CommandConfig& config, const CommandCallback& callback) {
    CommandConfig cfg = config;
    if (cfg.requestId.empty()) {
        cfg.requestId = generateRequestId();
    }

    NET_LOG_INFO("CommandRequest", "Sending command: cmdId=" + std::to_string(cfg.cmdId) +
                  " cmd=" + cfg.cmd + " requestId=" + cfg.requestId);

    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        // 在 DLL 侧拷贝 callback：确保 std::function 及其捕获对象由 DLL 的 CRT 堆分配/释放。
        // 若直接按值传参，EXE 构造的 std::function 会由 DLL 析构，静态 CRT 下会破坏堆。
        pendingRequests_[cfg.requestId] = {cfg, callback, std::chrono::steady_clock::now()};
    }

    // 在锁内查找并拷贝 handler，避免与 setRequestHandler/removeRequestHandler 并发访问 map。
    std::function<CommandResult(const CommandConfig&)> handler;
    {
        std::lock_guard<std::mutex> lock(handlersMutex_);
        auto handlerIt = handlers_.find(cfg.cmdId);
        if (handlerIt != handlers_.end()) {
            handler = handlerIt->second;
        }
    }
    if (!handler) {
        CommandResult result;
        result.requestId = cfg.requestId;
        result.cmdId = cfg.cmdId;
        result.code = -1;
        result.message = "No handler for cmdId=" + std::to_string(cfg.cmdId);
        NET_LOG_ERROR("CommandRequest", result.message);
        onResponse(cfg.requestId, result);
        return;
    }


    auto executeTask = [this, cfg, handler]() {
        CommandResult result;
        try {
            result = handler(cfg);
            result.requestId = cfg.requestId;
            result.cmdId = cfg.cmdId;
        } catch (const std::exception& e) {
            result.requestId = cfg.requestId;
            result.cmdId = cfg.cmdId;
            result.code = -1;
            result.message = std::string("Handler exception: ") + e.what();
            NET_LOG_ERROR("CommandRequest", result.message);
        }
        onResponse(cfg.requestId, result);
    };

    if (threadPool_) {
        threadPool_->submitVoid(executeTask);
    } else {
        executeTask();
    }
}

void CommandRequest::send(int cmdId, const std::string& cmd, const CommandCallback& callback, int timeoutMs) {
    CommandConfig config;
    config.cmdId = cmdId;
    config.cmd = cmd;
    config.timeoutMs = timeoutMs;
    config.requestId = generateRequestId();
    send(config, callback);
}

void CommandRequest::send(int cmdId, const std::string& cmd, const std::string& jsonParams, const CommandCallback& callback, int timeoutMs) {
    CommandConfig config;
    config.cmdId = cmdId;
    config.cmd = cmd;
    config.timeoutMs = timeoutMs;
    config.requestId = generateRequestId();
    config.body = jsonParams;
    send(config, callback);
}

void CommandRequest::cancel(const std::string& requestId) {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pendingRequests_.erase(requestId);
    NET_LOG_INFO("CommandRequest", "Cancelled request: " + requestId);
}

void CommandRequest::cancelAll() {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    int count = static_cast<int>(pendingRequests_.size());
    pendingRequests_.clear();
    NET_LOG_INFO("CommandRequest", "Cancelled all " + std::to_string(count) + " pending requests");
}

bool CommandRequest::isPending(const std::string& requestId) const {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    return pendingRequests_.find(requestId) != pendingRequests_.end();
}

int CommandRequest::getPendingCount() const {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    return static_cast<int>(pendingRequests_.size());
}

void CommandRequest::setRequestHandler(int cmdId, const std::function<CommandResult(const CommandConfig&)>& handler) {
    std::lock_guard<std::mutex> lock(handlersMutex_);
    handlers_[cmdId] = handler;
    NET_LOG_DEBUG("CommandRequest", "Handler registered for cmdId=" + std::to_string(cmdId));
}

void CommandRequest::removeRequestHandler(int cmdId) {
    std::lock_guard<std::mutex> lock(handlersMutex_);
    handlers_.erase(cmdId);
    NET_LOG_DEBUG("CommandRequest", "Handler removed for cmdId=" + std::to_string(cmdId));
}

void CommandRequest::onResponse(const std::string& requestId, const CommandResult& result) {
    CommandCallback callback;
    CallbackPoster poster;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        auto it = pendingRequests_.find(requestId);
        if (it == pendingRequests_.end()) {
            NET_LOG_WARN("CommandRequest", "Response for unknown requestId: " + requestId);
            return;
        }
        callback = it->second.callback;
        pendingRequests_.erase(it);
    }
    {
        std::lock_guard<std::mutex> lock(posterMutex_);
        poster = callbackPoster_;
    }

    NET_LOG_INFO("CommandRequest", "Response received: requestId=" + requestId +
                  " code=" + std::to_string(result.code));

    // v1.3.0：注入的宿主投递器优先（真正切到宿主 UI 主线程），否则回退旧行为。
    std::function<void()> task = [callback, result]() {
        if (callback) callback(result);
    };
    if (poster) {
        poster(std::move(task));
    } else if (mainThreadDispatcher_) {
        mainThreadDispatcher_->post(task);
    } else {
        task();
    }
}

void CommandRequest::sendWithJsonResult(int cmdId, const std::string& cmd, const std::string& jsonParams,
                                        const std::function<void(const std::string&)>& onSuccess,
                                        const std::function<void(NetErrorCode, const std::string&)>& onError,
                                        int timeoutMs) {
    CommandConfig config;
    config.cmdId = cmdId;
    config.cmd = cmd;
    config.timeoutMs = timeoutMs;
    config.requestId = generateRequestId();
    config.body = jsonParams;

    send(config, [onSuccess, onError](const CommandResult& result) {
        if (result.ok()) {
            if (onSuccess) onSuccess(result.rawData);
        } else {
            if (onError) onError(NetErrorCode::CommandFailed, result.message);
        }
    });
}

void CommandRequest::checkTimeout() {
    auto now = std::chrono::steady_clock::now();
    std::vector<std::string> timeoutIds;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        for (auto& [id, req] : pendingRequests_) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - req.startTime);
            if (elapsed.count() >= req.config.timeoutMs) {
                timeoutIds.push_back(id);
            }
        }
    }
    for (const auto& id : timeoutIds) {
        CommandResult result;
        result.requestId = id;
        result.code = -2;
        result.message = "Request timeout";
        NET_LOG_WARN("CommandRequest", "Request timeout: " + id);
        onResponse(id, result);
    }
}

} // namespace net_sdk