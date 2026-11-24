/**
 * @file ThreadPool.h
 * @brief NetSDK 线程池
 *
 * 提供固定大小的线程池，支持异步任务提交和 future 返回值获取。
 * 用于 HttpClient 异步请求、CommandRequest 异步命令等场景的并发执行。
 *
 * 典型用法：
 * @code
 *   ThreadPool pool(4);
 *   auto future = pool.submit([]() { return 42; });
 *   int result = future.get();
 * @endcode
 *
 * @author NetSDK Team
 * @version 1.0.0
 * @date 2026-11-18
 */

#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <future>
#include <type_traits>
#include <stdexcept>

namespace net_sdk {

/**
 * @brief 固定大小线程池
 *
 * 线程池在构造时创建指定数量的工作线程，通过 submit() 方法提交任务。
 * 任务以 std::function<void()> 形式存储在队列中，工作线程从队列中取出并执行。
 * 支持通过 std::future 获取任务返回值。
 */
class ThreadPool {
public:
    /**
     * @brief 构造线程池
     * @param threadCount 工作线程数量（默认 4）
     */
    explicit ThreadPool(int threadCount = 4);

    /// 析构线程池，会等待所有任务完成后销毁
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /// 提交无返回值任务（非模板版本，避免模板递归深度问题）
    void submitVoid(std::function<void()> task);

    /**
     * @brief 提交异步任务（模板版本，实现见 ThreadPool_impl.h）
     * @tparam F 可调用对象类型
     * @tparam Args 参数类型
     * @param f 可调用对象
     * @param args 调用参数
     * @return std::future 包装的任务返回值
     */
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>;

    /// 关闭线程池，停止接收新任务并等待所有任务完成
    void shutdown();

    /// 获取线程数量
    int getThreadCount() const;

    /// 获取待执行任务数量
    int getPendingTaskCount() const;

private:
    void workerLoop();

    std::vector<std::thread> threads_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::atomic<bool> stopped_{false};
    int threadCount_;
};

} // namespace net_sdk