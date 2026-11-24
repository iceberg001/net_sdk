#include "net_sdk/ThreadPool.h"
#include "net_sdk/Logger.h"

namespace net_sdk {

ThreadPool::ThreadPool(int threadCount) : threadCount_(threadCount) {
    for (int i = 0; i < threadCount; ++i) {
        threads_.emplace_back(&ThreadPool::workerLoop, this);
    }
    NET_LOG_DEBUG("ThreadPool", "ThreadPool created with " + std::to_string(threadCount) + " threads");
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::workerLoop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return stopped_ || !tasks_.empty(); });
            if (stopped_ && tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        try {
            task();
        } catch (const std::exception& e) {
            NET_LOG_ERROR("ThreadPool", std::string("Task exception: ") + e.what());
        }
    }
}

void ThreadPool::submitVoid(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            throw std::runtime_error("ThreadPool is stopped");
        }
        tasks_.emplace(std::move(task));
    }
    condition_.notify_one();
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_.exchange(true)) return; // 幂等：重复调用直接返回
    }
    condition_.notify_all();
    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    threads_.clear();
    NET_LOG_DEBUG("ThreadPool", "ThreadPool shutdown");
}

int ThreadPool::getThreadCount() const {
    return threadCount_;
}

int ThreadPool::getPendingTaskCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(tasks_.size());
}

} // namespace net_sdk