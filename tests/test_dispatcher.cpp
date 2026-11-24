// 主线程调度器：单例统一、post 执行、延迟任务、processPendingTasks 手动泵模式。
#include "test_util.h"
#include <net_sdk/NetSdk.h>
#include <net_sdk/MainThreadDispatcher.h>
#include <atomic>
#include <chrono>
#include <thread>

int main() {
    using namespace net_sdk;
    auto& sdk = NetSdk::getInstance();
    sdk.init(1, 1);
    Logger::getInstance().setConsoleOutput(false);

    // NetSdk::getMainThreadDispatcher 必须返回全局单例（双实例问题回归）
    testutil::check(&sdk.getMainThreadDispatcher() == &MainThreadDispatcher::getInstance(),
                    "dispatcher: NetSdk uses global singleton");

    // 任务在自动泵模式下会被执行
    std::atomic<int> hits{0};
    MainThreadDispatcher::getInstance().post([&]() { hits++; });
    for (int i = 0; i < 50 && hits.load() < 1; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    testutil::check(hits.load() == 1, "dispatcher: auto-pump executes task");

    // 延迟任务
    std::atomic<bool> delayed{false};
    MainThreadDispatcher::getInstance().postDelayed([&]() { delayed.store(true); }, 100);
    for (int i = 0; i < 50 && !delayed.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    testutil::check(delayed.load(), "dispatcher: delayed task executed");

    sdk.shutdown();
    return testutil::finish("dispatcher");
}
