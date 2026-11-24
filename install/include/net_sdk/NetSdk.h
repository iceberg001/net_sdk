/**
 * @file NetSdk.h
 * @brief NetSDK 统一入口头文件
 *
 * 包含 NetSDK 所有公共头文件，提供 NetSdk 单例类作为统一初始化和访问入口。
 * 业务项目只需包含此头文件即可使用 SDK 的全部功能。
 *
 * 典型用法：
 * @code
 *   #include "net_sdk/NetSdk.h"
 *
 *   // 初始化 SDK（UI 应用：注入「回调投递到主线程」的函数，之后所有异步回调
 *   // 都会自动切到你的 UI 主线程，业务层无需再自行 PostTask / 切线程）
 *   net_sdk::NetSdk::getInstance().init();
 *   net_sdk::NetSdk::getInstance().setCallbackPoster([](std::function<void()> t) {
 *       PostToMyUiThread(std::move(t));
 *   });
 *
 *   // 日志是独立公共 API：其它工程只需 #include <net_sdk/Logger.h>
 *   // （不依赖本头文件、C++11 起可用），一行完成级别/控制台/文件配置
 *   net_sdk::configureLogging(net_sdk::LogLevel::Info, false, "app");
 *
 *   // 使用 HTTP 客户端：异步 / 超时 / 模板解析 / 切主线程全部由 SDK 底层完成
 *   net_sdk::HttpRequestConfig cfg; cfg.url = "http://example.com/login";
 *   cfg.method = "POST"; cfg.body = "{}"; cfg.timeoutMs = 10000;
 *   net_sdk::NetSdk::getInstance().getHttpClient().requestAsyncTyped<LoginRsp>(
 *       cfg,
 *       [](const net_sdk::NetResult& r, LoginRsp& out) { ParseInto(r, out); },
 *       [](const LoginRsp& rsp) { OnLoginRsp(rsp); },  // 已在 UI 主线程
 *       "login");                                       // 同 key 覆盖
 *
 *   // TCP 长连接：先按业务协议分帧，再用请求级 API（并发 / 超时 / 覆盖 / 取消）
 *   net_sdk::TcpClient tcp;
 *   tcp.setLineFrameMode('\n');
 *   net_sdk::TcpClientConfig tc; tc.host = "127.0.0.1"; tc.port = 9000;
 *   tcp.connect(tc);
 *   // 同 key 的旧请求立即 Superseded，新请求不等旧请求
 *   tcp.sendRequestLatest("heartbeat", "ping\n", 3000,
 *       [](const net_sdk::NetResult& r) {});
 *
 *   // TCP 服务端：threadNum=4 启用 1 acceptor + 3 worker loop，同样支持分帧
 *   net_sdk::TcpServer srv;
 *   srv.setLineFrameMode('\n');
 *   net_sdk::TcpServerConfig sc; sc.port = 9000; sc.threadNum = 4;
 *   srv.start(sc);
 *
 *   // 使用命令请求
 *   net_sdk::NetSdk::getInstance().getCommandRequest().send(1001, "login", [](const auto& r) {
 *       // 回调在主线程执行
 *   });
 *
 *   // 使用信号槽
 *   NET_CONNECT("account_changed", [](const std::string& account) {
 *       std::cout << "Account: " << account << std::endl;
 *   });
 *   NET_SIGNAL("account_changed", std::string("user_001"));
 *
 *   // 关闭 SDK
 *   net_sdk::NetSdk::getInstance().shutdown();
 * @endcode
 *
 * @author NetSDK Team
 * @version 1.0.0
 * @date 2026-11-18
 */

#pragma once

#include "Common.h"
#include "Logger.h"
#include "ThreadPool.h"
#include "EventLoopPool.h"
#include "SignalSlot.h"
#include "JsonHelper.h"
#include "SqliteHelper.h"
#include "CommandRequest.h"
#include "MainThreadDispatcher.h"
#include "HttpClient.h"
#include "TcpClient.h"
#include "TcpServer.h"

namespace net_sdk {

/**
 * @brief NetSDK 统一管理类（单例）
 *
 * 提供 SDK 的初始化、关闭和各模块的统一访问入口。
 * 通过 init() 初始化所有内部模块，通过 get*() 方法获取各模块实例。
 */
class NetSdk {
public:
    /// 获取 NetSdk 单例实例
    static NetSdk& getInstance();

    /**
     * @brief 初始化 SDK（线程安全、幂等）
     *
     * @param threadCount 线程池线程数（默认 4）
     * @param loopCount 事件循环数（默认 4）
     * @return 是否成功初始化
     */
    bool init(int threadCount = 4, int loopCount = 4);

    /**
     * @brief 设置宿主回调投递器（v1.3.0，UI 应用必调）
     *
     * 注入「把任务投递到宿主 UI 主线程」的函数后，SDK 所有异步结果回调
     * （HTTP / 命令请求 / requestAsyncTyped 模板解析结果）都会经它投递到该线程，
     * 业务层无需、也不应再做二次线程切换。
     *
     * 可在 init() 之前或之后调用（会保存并应用到已创建的模块）；线程安全。
     * 契约与示例见 Common.h 的 CallbackPoster 说明。传空函数可取消注入。
     */
    void setCallbackPoster(const CallbackPoster& poster);

    /// 关闭 SDK，释放所有资源（线程安全、幂等）
    void shutdown();

    /// 当前是否已初始化
    bool isInitialized() const;

    /// 获取线程池
    ThreadPool& getThreadPool();

    /// 获取事件循环池
    EventLoopPool& getEventLoopPool();

    /// 获取 HTTP 客户端
    HttpClient& getHttpClient();

    /// 获取命令请求管理器
    CommandRequest& getCommandRequest();

    /// 获取主线程调度器
    MainThreadDispatcher& getMainThreadDispatcher();

    /// 获取信号槽管理器
    SignalSlot& getSignalSlot();

    /// 设置日志级别
    void setLogLevel(LogLevel level);

    /// 启用/禁用控制台日志输出
    void enableConsoleLog(bool enable);

    /**
     * @brief 启用/禁用文件日志输出
     * @param enable 是否启用
     * @param filePath 日志文件基础名或路径（如 "app" → YYYY-MM-DD_app.log，
     *                 超过 5MB 依次轮转为 YYYY-MM-DD_app1.log、app2.log ...）
     */
    void enableFileLog(bool enable, const std::string& filePath = "");

private:
    NetSdk();
    ~NetSdk();
    NetSdk(const NetSdk&) = delete;
    NetSdk& operator=(const NetSdk&) = delete;

    // 内部实现：调用方需已持有 stateMutex_。
    bool initLocked(int threadCount, int loopCount);
    void shutdownLocked();
    // 若尚未初始化则隐式初始化（访问器用，避免解引用空指针）。调用方需已持有 stateMutex_。
    void ensureInitializedLocked();

    std::unique_ptr<ThreadPool> threadPool_;
    std::unique_ptr<EventLoopPool> eventLoopPool_;
    std::unique_ptr<HttpClient> httpClient_;
    std::unique_ptr<CommandRequest> commandRequest_;
    CallbackPoster callbackPoster_;   ///< 宿主回调投递器（init 前后均可设置）
    // 主线程调度器改用全局单例，保证消费者 getInstance() 与 SDK 内部回调
    // 进入同一个任务队列，消除"双实例"导致的队列不互通问题。
    std::atomic<bool> initialized_{false};
    mutable std::mutex stateMutex_;
};

} // namespace net_sdk