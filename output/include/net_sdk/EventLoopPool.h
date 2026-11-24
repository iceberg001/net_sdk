/**
 * @file EventLoopPool.h
 * @brief NetSDK 事件循环池
 *
 * 基于 libhv 的 hloop_t 实现的多线程事件循环池，每个循环在独立线程中运行。
 * 用于 TcpClient、TcpServer 等需要事件驱动的模块，提供负载均衡的循环分配。
 *
 * @author NetSDK Team
 * @version 1.0.0
 * @date 2026-11-18
 */

#pragma once

#include <memory>
#include <vector>
#include <atomic>
#include <functional>

namespace net_sdk {

/**
 * @brief 事件循环池
 *
 * 创建多个 hloop_t 事件循环，每个循环在独立线程中运行。
 * 通过 getNextLoop() 以轮询方式分配循环，实现负载均衡。
 */
class EventLoopPool {
public:
    /**
     * @brief 构造事件循环池
     * @param loopCount 事件循环数量（默认 4）
     */
    explicit EventLoopPool(int loopCount = 4);

    ~EventLoopPool();

    EventLoopPool(const EventLoopPool&) = delete;
    EventLoopPool& operator=(const EventLoopPool&) = delete;

    /// 启动所有事件循环线程
    void start();

    /// 停止所有事件循环线程
    void stop();

    /// 是否正在运行
    bool isRunning() const;

    /// 以轮询方式获取下一个事件循环（线程安全）
    void* getNextLoop();

    /// 获取指定索引的事件循环
    void* getLoop(int index);

    /// 获取事件循环数量
    int getLoopCount() const;

private:
    int loopCount_;
    std::atomic<bool> running_{false};
    std::atomic<int> nextIndex_{0};
    void* loopsData_ = nullptr;
    std::vector<void*> loops_;
};

} // namespace net_sdk