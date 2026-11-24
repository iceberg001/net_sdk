/**
 * @file ThreadPool_impl.h
 * @brief ThreadPool 模板实现（submit 方法）
 *
 * 将 submit 模板方法分离到此头文件，避免在不需要 future 返回值的
 * 翻译单元中引入 std::invoke_result_t / std::packaged_task 等重模板，
 * 从而规避 MSVC C1202（递归类型依赖上下文太复杂）编译错误。
 *
 * 仅在需要调用 ThreadPool::submit() 的源文件中包含此头文件。
 *
 * @author NetSDK Team
 * @version 1.0.0
 * @date 2026-11-18
 */

#pragma once

#include "ThreadPool.h"
#include <future>
#include <memory>

namespace net_sdk {

template<typename F, typename... Args>
auto ThreadPool::submit(F&& f, Args&&... args) {
    using ReturnType = std::invoke_result_t<F, Args...>;
    auto task = std::make_shared<std::packaged_task<ReturnType()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );
    std::future<ReturnType> result = task->get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            throw std::runtime_error("ThreadPool is stopped");
        }
        tasks_.emplace([task]() { (*task)(); });
    }
    condition_.notify_one();
    return result;
}

} // namespace net_sdk