/**
 * @file CommandRequest.h
 * @brief NetSDK 通用命令请求模块
 *
 * 基于 cmd/cmdId 的通用命令请求封装，支持异步发送、超时管理、
 * JSON 协议自动解析、回调线程可配置。适用于业务层统一接口请求。
 *
 * 核心特性：
 * - 内部异步执行，回调经 setCallbackPoster() 投递到宿主指定线程
 *   （未注入时回退 MainThreadDispatcher 泵线程，见 NetSdk.h 线程模型说明）
 * - 支持 cmdId 路由，不同命令由不同 handler 处理
 * - 支持超时管理和请求取消
 * - 支持 JSON 协议自动解析（sendWithJsonResult）
 *
 * 典型用法：
 * @code
 *   // 注册命令处理器
 *   cmdReq.setRequestHandler(1001, [](const CommandConfig& cfg) {
 *       CommandResult result;
 *       result.code = 0;
 *       result.rawData = R"({"token":"abc123"})";
 *       return result;
 *   });
 *
 *   // 发送命令并获取 JSON 结果（回调在主线程）
 *   cmdReq.sendWithJsonResult(1001, "login", R"({"user":"admin"})",
 *       [](const std::string& jsonStr) {
 *           // 在主线程执行，jsonStr 是回包 JSON 字符串
 *           auto j = JsonHelper::parse(jsonStr);
 *           std::string token = JsonHelper::get<std::string>(j, "token");
 *       });
 * @endcode
 *
 * @author NetSDK Team
 * @version 1.0.0
 * @date 2026-11-18
 */

#pragma once

#include "Common.h"
#include "JsonHelper.h"
#include <memory>
#include <map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <exception>
#include <thread>

namespace net_sdk {

class ThreadPool;
class MainThreadDispatcher;

/**
 * @brief 通用命令请求管理器
 *
 * 统一管理基于 cmd/cmdId 的命令请求生命周期，包括发送、超时、回调。
 * 请求内部异步执行；回调优先走宿主投递器（setCallbackPoster），
 * 未注入时回退到 MainThreadDispatcher。
 */
class CommandRequest {
public:
    CommandRequest();
    ~CommandRequest();

    CommandRequest(const CommandRequest&) = delete;
    CommandRequest& operator=(const CommandRequest&) = delete;

    /// 设置线程池（用于异步执行命令 handler）
    void setThreadPool(ThreadPool* pool);

    /// 设置主线程调度器（未注入 poster 时的回退路径）
    void setMainThreadDispatcher(MainThreadDispatcher* dispatcher);

    /**
     * @brief 设置宿主回调投递器（v1.3.0）
     *
     * 注入后命令结果回调经它投递到宿主指定线程（通常是 UI 主线程），
     * 未注入时回退到 MainThreadDispatcher。契约见 Common.h 的 CallbackPoster。
     */
    void setCallbackPoster(const CallbackPoster& poster);

    /**
     * @brief 发送命令请求
     * @param config 命令配置（cmdId, cmd, timeout, requestId, body）
     * @param callback 回调函数（在主线程执行）
     */
    void send(const CommandConfig& config, const CommandCallback& callback);

    /**
     * @brief 发送命令请求（简化版）
     * @param cmdId 命令 ID
     * @param cmd 命令名称
     * @param callback 回调函数
     * @param timeoutMs 超时时间（毫秒）
     */
    void send(int cmdId, const std::string& cmd, const CommandCallback& callback, int timeoutMs = 30000);

    /**
     * @brief 发送命令请求（带 JSON 字符串参数）
     * @param cmdId 命令 ID
     * @param cmd 命令名称
     * @param jsonParams JSON 格式参数字符串
     * @param callback 回调函数
     * @param timeoutMs 超时时间
     */
    void send(int cmdId, const std::string& cmd, const std::string& jsonParams, const CommandCallback& callback, int timeoutMs = 30000);

    /**
     * @brief 取消指定请求
     * @param requestId 请求 ID
     */
    void cancel(const std::string& requestId);

    /// 取消所有待处理请求
    void cancelAll();

    /// 检查指定请求是否仍在等待
    bool isPending(const std::string& requestId) const;

    /// 获取待处理请求数量
    int getPendingCount() const;

    /**
     * @brief 注册命令处理器
     * @param cmdId 命令 ID
     * @param handler 处理函数（接收 CommandConfig，返回 CommandResult）
     */
    void setRequestHandler(int cmdId, const std::function<CommandResult(const CommandConfig&)>& handler);

    /**
     * @brief 移除命令处理器
     * @param cmdId 命令 ID
     */
    void removeRequestHandler(int cmdId);

    /**
     * @brief 处理命令响应（内部调用，将结果投递到主线程回调）
     * @param requestId 请求 ID
     * @param result 命令结果
     */
    void onResponse(const std::string& requestId, const CommandResult& result);

    /**
     * @brief 发送命令并返回 JSON 结果字符串
     *
     * 回包的 JSON 数据通过 onSuccess 回调以字符串形式返回。
     * 如需映射到结构体，在回调中调用 JsonHelper::parse() 和 JsonHelper::fromJson<T>() 即可。
     * 回调均在主线程执行。
     *
     * @param cmdId 命令 ID
     * @param cmd 命令名称
     * @param jsonParams JSON 格式参数字符串
     * @param onSuccess 成功回调（接收 JSON 字符串）
     * @param onError 失败回调（接收错误码和错误信息）
     * @param timeoutMs 超时时间
     */
    void sendWithJsonResult(int cmdId, const std::string& cmd, const std::string& jsonParams,
                            const std::function<void(const std::string&)>& onSuccess,
                            const std::function<void(NetErrorCode, const std::string&)>& onError = nullptr,
                            int timeoutMs = 30000);

    /**
     * @brief 发送命令并将回包 JSON 自动反序列化为指定结构体
     *
     * 回调均在主线程执行。目标结构体需定义 NLOHMANN_DEFINE_TYPE_INTRUSIVE 等映射宏。
     *
     * @tparam T 目标结构体类型
     * @param cmdId 命令 ID
     * @param cmd 命令名称
     * @param jsonParams 请求参数（Json 对象）
     * @param onSuccess 成功回调（接收反序列化后的结构体）
     * @param onError 失败回调（可选）
     * @param timeoutMs 超时时间（毫秒）
     */
    template<typename T>
    void sendAndParse(int cmdId, const std::string& cmd, const Json& jsonParams,
                      const std::function<void(const T&)>& onSuccess,
                      const std::function<void(NetErrorCode, const std::string&)>& onError = nullptr,
                      int timeoutMs = 30000) {
        sendWithJsonResult(cmdId, cmd, jsonParams.dump(),
            [onSuccess, onError](const std::string& rawJson) {
                bool ok = false;
                Json j = JsonHelper::parse(rawJson, &ok);
                if (!ok) {
                    if (onError) onError(NetErrorCode::JsonParseError, "Invalid JSON response");
                    return;
                }
                try {
                    T obj = JsonHelper::fromJson<T>(j);
                    if (onSuccess) onSuccess(obj);
                } catch (const std::exception& e) {
                    if (onError) onError(NetErrorCode::JsonParseError,
                                         std::string("JSON mapping failed: ") + e.what());
                }
            },
            onError, timeoutMs);
    }

private:
    std::string generateRequestId();
    void checkTimeout();

    ThreadPool* threadPool_ = nullptr;
    MainThreadDispatcher* mainThreadDispatcher_ = nullptr;
    CallbackPoster callbackPoster_;              ///< v1.3.0：宿主回调投递器
    mutable std::mutex posterMutex_;
    std::map<int, std::function<CommandResult(const CommandConfig&)>> handlers_;
    mutable std::mutex handlersMutex_;

    struct PendingRequest {
        CommandConfig config;
        CommandCallback callback;
        std::chrono::steady_clock::time_point startTime;
    };
    std::map<std::string, PendingRequest> pendingRequests_;
    mutable std::mutex pendingMutex_;
    std::atomic<int> requestCounter_{0};

    void timeoutLoop();                        ///< 后台超时检测线程
    std::unique_ptr<std::thread> timeoutThread_;
    std::atomic<bool> timeoutRunning_{false};
};

} // namespace net_sdk