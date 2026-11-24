#include "net_sdk/EventLoopPool.h"
#include "net_sdk/Logger.h"
#include "libhv/hloop.h"
#include <thread>

namespace net_sdk {

struct LoopThreadData {
    hloop_t* loop = nullptr;
    std::thread thread;
};

EventLoopPool::EventLoopPool(int loopCount) : loopCount_(loopCount) {
    NET_LOG_DEBUG("EventLoopPool", "EventLoopPool created with " + std::to_string(loopCount) + " loops");
}

EventLoopPool::~EventLoopPool() {
    stop();
}

void EventLoopPool::start() {
    if (running_.load()) return;
    running_.store(true);

    loopsData_ = new LoopThreadData[loopCount_];
    auto* loopThreads = (LoopThreadData*)loopsData_;

    for (int i = 0; i < loopCount_; ++i) {
        loopThreads[i].loop = hloop_new(HLOOP_FLAG_AUTO_FREE);
        hloop_t* loop = loopThreads[i].loop;
        loopThreads[i].thread = std::thread([loop]() {
            hloop_run(loop);
        });
        loops_.push_back(loop);
    }
    NET_LOG_INFO("EventLoopPool", "EventLoopPool started with " + std::to_string(loopCount_) + " loops");
}

void EventLoopPool::stop() {
    if (!running_.load()) return;
    running_.store(false);

    auto* loopThreads = (LoopThreadData*)loopsData_;
    for (int i = 0; i < loopCount_; ++i) {
        if (loopThreads[i].loop) {
            hloop_stop(loopThreads[i].loop);
        }
    }
    for (int i = 0; i < loopCount_; ++i) {
        if (loopThreads[i].thread.joinable()) {
            loopThreads[i].thread.join();
        }
    }
    delete[] loopThreads;
    loopsData_ = nullptr;
    loops_.clear();
    NET_LOG_INFO("EventLoopPool", "EventLoopPool stopped");
}

bool EventLoopPool::isRunning() const {
    return running_.load();
}

void* EventLoopPool::getNextLoop() {
    if (loops_.empty()) return nullptr;
    int index = nextIndex_.fetch_add(1) % loopCount_;
    return loops_[index];
}

void* EventLoopPool::getLoop(int index) {
    if (index < 0 || index >= static_cast<int>(loops_.size())) return nullptr;
    return loops_[index];
}

int EventLoopPool::getLoopCount() const {
    return loopCount_;
}

} // namespace net_sdk