/**
 * @file TcpServer.h
 * @brief NetSDK TCP 服务端
 *
 * 基于 libhv 的 hloop_t 实现 TCP 服务端，支持多连接管理、
 * 消息广播、连接/断开回调等功能。
 *
 * 并发与超时（v1.2.0 起）：
 * - threadNum > 1 时启用 one-acceptor-multi-workers：1 个 acceptor loop
 *   负责 accept，其余 worker loop 各自处理一批连接，多客户端真正并行；
 *   threadNum = 1 时 acceptor loop 兼作 worker（单 loop，默认）；
 * - readTimeoutMs / enableHeartbeat+heartbeatIntervalMs 对每个连接生效；
 * - send/broadcast/closeConnection 会投递到「连接所属 loop」并在 loop 内
 *   重新校验连接存活，避免连接已关闭后对悬空 hio_t* 写入。
 *
 * 线程模型：
 * - 所有 libhv IO 回调在该连接所属的事件循环线程执行；
 * - 每连接上下文以值存于服务端状态表，地址稳定，不依赖 hloop userdata，
 *   因此多个 TcpServer 可安全共存；
 * - 对象析构后，在途回调通过生命周期令牌安全跳过，无 use-after-free。
 *
 * 典型用法：
 * @code
 *   TcpServer server;
 *   server.setMessageCallback([](const std::string& data) {
 *       std::cout << "Received: " << data << std::endl;
 *   });
 *   server.setConnectionCallback([](int connId) {
 *       std::cout << "New connection: " << connId << std::endl;
 *   });
 *
 *   TcpServerConfig config;
 *   config.host = "0.0.0.0";
 *   config.port = 12345;
 *   server.start(config);
 *
 *   server.broadcast("Hello all clients!");
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
#include <map>
#include <mutex>

namespace net_sdk {

/**
 * @brief TCP 服务端
 *
 * 支持多客户端连接管理，提供消息广播、指定连接发送、连接/断开回调等。
 * 所有回调在事件循环线程中执行。
 */
class TcpServer {
public:
    TcpServer();
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    /**
     * @brief 启动 TCP 服务端
     * @param config 服务端配置
     * @return 是否成功启动
     */
    bool start(const TcpServerConfig& config);

    /// 停止服务端（线程安全）
    void stop();

    /// 是否正在运行
    bool isRunning() const;

    /**
     * @brief 向指定连接发送数据（线程安全）
     * @param connId 连接 ID
     * @param data 待发送数据
     * @return 是否成功投递发送任务
     */
    bool send(int connId, const std::string& data);

    /**
     * @brief 向指定连接发送原始数据（线程安全）
     * @param connId 连接 ID
     * @param data 数据指针
     * @param len 数据长度
     * @return 是否成功投递发送任务
     */
    bool send(int connId, const char* data, size_t len);

    /**
     * @brief 向所有连接广播数据（线程安全）
     * @param data 待广播数据
     */
    void broadcast(const std::string& data);

    /**
     * @brief 关闭指定连接（线程安全）
     * @param connId 连接 ID
     */
    void closeConnection(int connId);

    /// 获取当前连接数
    int getConnectionCount() const;

    /// 获取所有连接 ID 列表
    std::vector<int> getConnectionIds() const;

    /// 设置消息接收回调
    void setMessageCallback(TcpMessageCallback callback);

    /// 设置新连接回调
    void setConnectionCallback(TcpConnectionCallback callback);

    /// 设置连接断开回调
    void setDisconnectionCallback(TcpConnectionCallback callback);

    /// 设置错误回调
    void setErrorCallback(TcpErrorCallback callback);

    /// 设置事件循环池（共享使用；须在 start 之前设置）
    void setEventLoopPool(EventLoopPool* pool);

    /**
     * @brief 设置帧解码器（处理 TCP 粘包 / 半包）
     *
     * 每个连接持有独立的累积缓冲；设置后 messageCallback 每次收到一个完整帧。
     * 多客户端并发收发时强烈建议启用，建议在 start() 之前设置。
     */
    void setFrameDecoder(const TcpFrameDecoder& decoder);

    /// 便捷：按分隔符分帧（默认 '\\n'，帧内容不含分隔符）
    void setLineFrameMode(char delimiter = '\n');

private:
    struct LifeToken {
        std::atomic<bool> alive{true};
    };

    // 内部状态（Pimpl）
    struct State;
    std::shared_ptr<State> state_;
    std::shared_ptr<LifeToken> life_;
};

} // namespace net_sdk
