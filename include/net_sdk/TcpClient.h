/**
 * @file TcpClient.h
 * @brief NetSDK TCP 客户端
 *
 * 基于 libhv 的 hloop_t 实现 TCP 客户端，支持长连接/短连接、
 * 自动重连、心跳保活、消息回调等功能。
 *
 * 并发与超时（v1.2.0 起）：
 * - 原始流式通道：send() / setMessageCallback()，仅保证线程安全与顺序写；
 * - 请求级通道：sendRequest() / sendRequestLatest()，每请求独立 watchdog 超时、
 *   同 key 覆盖、取消、在途上限背压；多个请求可同时在途，互不阻塞；
 * - 连接级：connect/read/write 三个超时 + 重连次数上限。
 *
 * 线程模型：
 * - 所有 libhv IO/定时器回调均在事件循环线程执行；
 * - 业务线程调用 send()/sendRequest()/disconnect() 等会安全投递到 loop 线程；
 * - 回调通过生命周期令牌保护，对象析构后不会触发悬空回调（无 use-after-free）。
 *
 * 典型用法：
 * @code
 *   TcpClient client;
 *   client.setMessageCallback([](const std::string& data) {
 *       std::cout << "Received: " << data << std::endl;
 *   });
 *   client.setConnectionCallback([](int connId) {
 *       if (connId > 0) std::cout << "Connected" << std::endl;
 *   });
 *
 *   TcpClientConfig config;
 *   config.host = "127.0.0.1";
 *   config.port = 12345;
 *   config.autoReconnect = true;
 *   client.connect(config);
 *
 *   client.send("Hello Server!");
 * @endcode
 *
 * @author NetSDK Team
 * @version 1.0.0
 * @date 2026-11-18
 */

#pragma once

#include "Common.h"
#include "EventLoopPool.h"
#include <memory>
#include <atomic>
#include <mutex>
#include <cstdint>

namespace net_sdk {

/**
 * @brief TCP 客户端
 *
 * 支持长连接和短连接模式，可配置自动重连、心跳保活。
 * 所有回调在事件循环线程中执行，如需更新 UI 请通过 MainThreadDispatcher 切换。
 */
class TcpClient {
public:
    TcpClient();
    ~TcpClient();

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    /**
     * @brief 连接到服务器（异步）
     * @param config 连接配置
     * @return 是否成功发起连接（true 不代表已连接成功）
     */
    bool connect(const TcpClientConfig& config);

    /// 断开连接（线程安全，可跨线程调用）
    void disconnect();

    /// 是否已连接
    bool isConnected() const;

    /// 获取连接 ID
    int getConnectionId() const;

    /**
     * @brief 发送数据（线程安全，内部序列化到 loop 线程）
     * @param data 待发送的字符串
     * @return 是否成功投递发送任务
     */
    bool send(const std::string& data);

    /**
     * @brief 发送原始数据（线程安全）
     * @param data 数据指针
     * @param len 数据长度
     * @return 是否成功投递发送任务
     */
    bool send(const char* data, size_t len);

    /// 设置消息接收回调
    void setMessageCallback(TcpMessageCallback callback);

    /// 设置连接状态回调（connId > 0 表示连接成功，connId < 0 表示断开）
    void setConnectionCallback(TcpConnectionCallback callback);

    /// 设置错误回调
    void setErrorCallback(TcpErrorCallback callback);

    /// 设置事件循环池（共享使用；须在 connect 之前设置）
    void setEventLoopPool(EventLoopPool* pool);

    /**
     * @brief 启用心跳保活
     * @param heartbeatData 心跳数据内容
     * @param intervalMs 心跳间隔（毫秒）
     */
    void enableHeartbeat(const std::string& heartbeatData, int intervalMs);

    /// 禁用心跳保活
    void disableHeartbeat();

    /**
     * @brief 设置自动重连
     * @param enable 是否启用
     * @param intervalMs 重连间隔（毫秒）
     */
    void setAutoReconnect(bool enable, int intervalMs = 3000);

    // ============ 请求级通道：并发在途 / 超时 / 同 key 覆盖 / 取消 ============

    /**
     * @brief 发送一帧并等待回包（带请求级超时，不阻塞后续请求）
     *
     * 多个请求可同时在途：本调用只投递一帧并登记一个 watchdog，
     * 不会等待上一个请求返回。收到回包时按 FIFO 或 matcher 配对；
     * 超时则回调 NetErrorCode::Timeout，迟到的回包被丢弃。
     * 连接在回包前断开时，所有在途请求立即回调 ConnectionClosed。
     *
     * @param data       请求帧
     * @param timeoutMs  请求级超时（毫秒，<=0 表示不设 watchdog）
     * @param callback   结果回调（在事件循环线程执行，更新 UI 请自行切主线程）
     * @param matcher    可选响应匹配器，为空时按 FIFO 配对
     * @return 请求 ID（>0）；0 表示未发出：未连接 / 对象已析构 / 达到在途上限
     */
    uint64_t sendRequest(const std::string& data, int timeoutMs,
                         const TcpResponseCallback& callback,
                         const TcpResponseMatcher& matcher = nullptr);

    /**
     * @brief 同 key 覆盖式请求（旧请求立即被取代，新请求立即发出）
     *
     * 用于「上一个请求超时未回包、新的同类请求又过来」的场景：
     * 同 key 的未完成旧请求立刻回调 NetErrorCode::Superseded 并取消其 watchdog，
     * 新请求不与旧请求排队、直接写入连接。
     *
     * @param key        业务键（如接口名 / cmdId）
     * @return 请求 ID（>0）；0 表示未发出
     */
    uint64_t sendRequestLatest(const std::string& key, const std::string& data,
                               int timeoutMs, const TcpResponseCallback& callback,
                               const TcpResponseMatcher& matcher = nullptr);

    /// 取消指定请求（成功受理返回 true；已结束的请求不再产生回调）
    bool cancelRequest(uint64_t requestId);

    /// 取消全部在途请求（每个都会收到一次 Canceled 回调）
    void cancelAllRequests();

    /// 当前在途请求数
    int getPendingRequestCount() const;

    /// 设置在途请求上限（背压）；默认取 TcpClientConfig::maxPendingRequests
    void setMaxPendingRequests(int n);

    /**
     * @brief 设置帧解码器（处理 TCP 粘包 / 半包）
     *
     * 强烈建议设置：多请求并发时，多个回包常被合并进一次 read，
     * 若不分帧，matcher 与 FIFO 配对都会失效。建议在 connect() 之前设置。
     */
    void setFrameDecoder(const TcpFrameDecoder& decoder);

    /// 便捷：按分隔符分帧（默认 '\\n'，帧内容不含分隔符）
    void setLineFrameMode(char delimiter = '\n');

private:
    // 请求级通道的内部实现（sendRequest / sendRequestLatest 共用）
    uint64_t sendRequestImpl(const std::string& key, const std::string& data,
                             int timeoutMs, const TcpResponseCallback& callback,
                             const TcpResponseMatcher& matcher);

    // 生命周期令牌：libhv 回调持有其 shared_ptr，析构时置 alive=false。
    // 回调在调用任何成员函数前 lock() 并校验 alive，避免对象销毁后的 UAF。
    struct LifeToken {
        std::atomic<bool> alive{true};
    };

    // 内部状态（Pimpl），包含所有供 libhv 回调访问的字段与方法。
    struct State;
    std::shared_ptr<State> state_;
    std::shared_ptr<LifeToken> life_;
};

} // namespace net_sdk
