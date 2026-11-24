#include "net_sdk/MainThreadDispatcher.h"
#include "net_sdk/Logger.h"
#include <chrono>

namespace net_sdk {

MainThreadDispatcher& MainThreadDispatcher::getInstance() {
    // 进程生命周期常驻，避免退出阶段与其它单例的析构顺序 UAF。
    static MainThreadDispatcher* instance = new MainThreadDispatcher();
    return *instance;
}

MainThreadDispatcher::MainThreadDispatcher() {}

MainThreadDispatcher::~MainThreadDispatcher() {
    shutdown();
}

void MainThreadDispatcher::init(bool autoPump) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (initialized_.load()) return;
    initialized_.store(true);
    autoPump_.store(autoPump);

    if (autoPump) {
        running_.store(true);
        // 内部泵线程：使 post/postDelayed 的任务无需宿主手动 pump 即可被处理。
        // UI 应用可传 autoPump=false，改由主线程消息循环调用 processPendingTasks()。
        thread_ = std::make_unique<std::thread>([this]() { pumpLoop(); });
    }
    NET_LOG_INFO("MainThreadDispatcher", "MainThreadDispatcher initialized (autoPump=" +
                  std::string(autoPump ? "true" : "false") + ")");
}

void MainThreadDispatcher::pumpLoop() {
    // 延迟任务没有外部事件唤醒，因此等待设上限，周期性检查到期任务。
    const auto kTick = std::chrono::milliseconds(16);
    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // 无谓词的定时等待：普通任务由 post()/postDelayed() 的 notify 即时唤醒，
            // 延迟任务由这里周期性检查到期时间；若谓词用 !taskQueue_.empty()，
            // 未到期的延迟任务会导致忙等。
            condition_.wait_for(lock, kTick);
        }
        processPendingTasks();
    }
    // 退出前清空剩余任务，避免析构时任务被静默丢弃
    processPendingTasks();
}

void MainThreadDispatcher::shutdown() {
    // 全程持锁：pumpLoop 不访问 stateMutex_，join 期间不会有锁环。
    // 持锁可防止并发 init() 在 join 窗口内重新启动线程。
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!initialized_.load()) return;
    initialized_.store(false);
    running_.store(false);
    condition_.notify_all();
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
    thread_.reset();
    NET_LOG_INFO("MainThreadDispatcher", "MainThreadDispatcher shutdown");
}

bool MainThreadDispatcher::isInitialized() const {
    return initialized_.load();
}

int64_t MainThreadDispatcher::getCurrentTimestampMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

void MainThreadDispatcher::post(const std::function<void()>& task) {
    if (!initialized_.load()) {
        NET_LOG_WARN("MainThreadDispatcher", "Dispatcher not initialized, executing task directly");
        task();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        taskQueue_.push({task, getCurrentTimestampMs()});
    }
    condition_.notify_one();
    NET_LOG_TRACE("MainThreadDispatcher", "Task posted to main thread");
}

void MainThreadDispatcher::postDelayed(const std::function<void()>& task, int delayMs) {
    if (!initialized_.load()) {
        NET_LOG_WARN("MainThreadDispatcher", "Dispatcher not initialized, executing task directly");
        task();
        return;
    }
    int64_t executeTime = getCurrentTimestampMs() + delayMs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        taskQueue_.push({task, executeTime});
    }
    condition_.notify_one();
    NET_LOG_TRACE("MainThreadDispatcher", "Delayed task posted, delay=" + std::to_string(delayMs) + "ms");
}

void MainThreadDispatcher::processPendingTasks() {
    std::queue<std::pair<std::function<void()>, int64_t>> readyTasks;
    int64_t now = getCurrentTimestampMs();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto tempQueue = std::move(taskQueue_);
        while (!tempQueue.empty()) {
            auto& item = tempQueue.front();
            if (item.second <= now) {
                readyTasks.push(item);
            } else {
                taskQueue_.push(item);
            }
            tempQueue.pop();
        }
    }
    while (!readyTasks.empty()) {
        auto& item = readyTasks.front();
        try {
            item.first();
        } catch (const std::exception& e) {
            NET_LOG_ERROR("MainThreadDispatcher", std::string("Task exception: ") + e.what());
        }
        readyTasks.pop();
    }
    std::function<void()> tick;
    {
        std::lock_guard<std::mutex> lock(tickMutex_);
        tick = tickCallback_;
    }
    if (tick) {
        tick();
    }
}

bool MainThreadDispatcher::hasPendingTasks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !taskQueue_.empty();
}

int MainThreadDispatcher::getPendingTaskCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(taskQueue_.size());
}

void MainThreadDispatcher::setTickCallback(const std::function<void()>& callback) {
    std::lock_guard<std::mutex> lock(tickMutex_);
    // callback 由调用方持有并析构；这里在 DLL 侧拷贝一份，避免跨 CRT 堆释放。
    tickCallback_ = callback;
}

} // namespace net_sdk