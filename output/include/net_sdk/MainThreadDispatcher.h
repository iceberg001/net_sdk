/**
 * @file MainThreadDispatcher.h
 * @brief NetSDK 主线程调度器
 *
 * 将异步操作的结果回调投递到「调度器线程」执行。内部维护一个任务队列，
 * 由内部泵线程或宿主主线程调用 processPendingTasks() 处理。
 *
 * 重要（v1.3.0 澄清）：autoPump=true（默认）时任务跑在 SDK **内部泵线程**，
 * 并不等于宿主 UI 主线程！若宿主有独立 UI 消息循环且要求回调严格在 UI 线程，
 * 推荐使用 NetSdk::setCallbackPoster() 注入宿主投递器（见 Common.h），
 * 而不是依赖本调度器；否则从 SDK 泵线程再向 UI 线程二次投递时，若用了非
 * 线程安全的 WeakPtrFactory / ScopedRunnableMethodFactory，任务会被静默丢弃，
 * 表现为「接口明明返回了，上层却收不到回调」。
 *
 * 典型用法：
 * @code
 *   // 主线程中初始化
 *   MainThreadDispatcher::getInstance().init();
 *
 *   // 异步线程中投递任务
 *   MainThreadDispatcher::getInstance().post([]() {
 *       // 此代码将在调度器线程中执行（autoPump 时为 SDK 内部泵线程）
 *       doWork();
 *   });
 *
 *   // autoPump=false 时，需在宿主主线程消息循环中定期调用
 *   MainThreadDispatcher::getInstance().processPendingTasks();
 * @endcode
 *
 * @author NetSDK Team
 * @version 1.0.0
 * @date 2026-11-18
 */

#pragma once

#include <functional>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <memory>

namespace net_sdk {

/**
 * @brief 主线程任务调度器（单例）
 *
 * 提供将任务从工作线程投递到「调度器线程」执行的能力。默认 autoPump=true
 * 时由 SDK 内部泵线程消费队列；UI 应用请改用 NetSdk::setCallbackPoster()
 * 把回调直接投递到宿主 UI 主线程（见文件头说明）。
 */
class MainThreadDispatcher {
public:
    /// 获取 MainThreadDispatcher 单例实例
    static MainThreadDispatcher& getInstance();

    MainThreadDispatcher();
    ~MainThreadDispatcher();

    MainThreadDispatcher(const MainThreadDispatcher&) = delete;
    MainThreadDispatcher& operator=(const MainThreadDispatcher&) = delete;

    /**
     * @brief 初始化调度器
     * @param autoPump 是否启动内部工作线程自动处理队列（默认 true）。
     *                 开箱即用场景保持 true，回调会被自动触发；
     *                 UI 应用若需回调严格运行在主线程，可传 false，
     *                 并在自己的主线程消息循环中调用 processPendingTasks()。
     */
    void init(bool autoPump = true);

    /// 关闭调度器
    void shutdown();

    /// 是否已初始化
    bool isInitialized() const;

    /**
     * @brief 投递任务到主线程
     * @param task 待执行的任务函数
     */
    void post(const std::function<void()>& task);

    /**
     * @brief 投递延迟任务到主线程
     * @param task 待执行的任务函数
     * @param delayMs 延迟时间（毫秒）
     */
    void postDelayed(const std::function<void()>& task, int delayMs);

    /**
     * @brief 处理所有待执行的任务
     *
     * 应在主线程的消息循环中定期调用此方法，以执行投递到主线程的任务。
     */
    void processPendingTasks();

    /// 是否有待执行的任务
    bool hasPendingTasks() const;

    /// 获取待执行任务数量
    int getPendingTaskCount() const;

    /// 设置每轮处理后的回调（可用于驱动 UI 刷新）
    void setTickCallback(const std::function<void()>& callback);

    /**
     * @brief 投递带参数的任务到主线程
     * @tparam F 可调用对象类型
     * @tparam Args 参数类型
     * @param f 可调用对象
     * @param args 调用参数
     */
    template<typename F, typename... Args>
    void post(F&& f, Args&&... args) {
        // 物化为 std::function<void()> 后必须显式选中非模板重载：
        // 直接写 post(task) 时模板的 F&& 对左值精确匹配，优于 const& 非模板，
        // 会再次进入本模板并无限递归（MSVC C4717 / 运行时卡死）。
        std::function<void()> task = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        using PostFn = void (MainThreadDispatcher::*)(const std::function<void()>&);
        (this->*static_cast<PostFn>(&MainThreadDispatcher::post))(task);
    }

private:
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::mutex stateMutex_;          // 保护 init/shutdown 的状态切换，防止并发重复起停线程
    std::queue<std::pair<std::function<void()>, int64_t>> taskQueue_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::unique_ptr<std::thread> thread_;
    std::function<void()> tickCallback_;
    mutable std::mutex tickMutex_;

    int64_t getCurrentTimestampMs() const;
    void pumpLoop();              ///< 自动泵线程主循环
    std::atomic<bool> autoPump_{false};
};

} // namespace net_sdk