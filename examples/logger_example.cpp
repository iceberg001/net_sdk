/**
 * @file logger_example.cpp
 * @brief 外部工程如何使用 NetSDK 的日志功能
 *
 * 关键点：日志是 SDK 的公共 API，且刻意做成「自包含、低依赖」：
 *   - 只需 #include <net_sdk/Logger.h>（不依赖 NetSdk.h 等其它 SDK 头）；
 *   - 该头只使用 C++11 标准库，以 /std:c++11 / /std:c++14 编译的
 *     既有工程也可以直接包含（无需把整个工程升级到 C++17）；
 *   - 链接时加入 net_sdk.lib / libnet_sdk.a 即可，不需要先 init() SDK。
 *
 * 构建方式（以本仓库为例）：
 *   cmake --build build --config Release --target logger_example
 */
#include <net_sdk/Logger.h>

#include <thread>
#include <vector>
#include <string>

int main() {
    using namespace net_sdk;

    // 一行完成配置：级别 / 控制台 / 文件日志。
    // "app" 会生成 YYYY-MM-DD_app.log，超过 5MB 依次轮转为
    // YYYY-MM-DD_app1.log、app2.log ...
    configureLogging(LogLevel::Debug, true, "app");
    configureLogRotation(5 * 1024 * 1024, 5);

    // 1) 经典「标签 + 字符串」
    NET_LOG_INFO("App", "application starting");
    NET_LOG_ERROR("App", "this is an error sample");

    // 2) 流式（推荐：任意 ostream 可输出类型，不必手工拼字符串）
    NET_LOG_I("App") << "stream log: uid=" << 1001 << ", name=" << "alice"
                     << ", ok=" << true;
    NET_LOG_D("App") << "debug detail: count=" << 42;

    // 3) printf 风格（便于从 C 风格日志迁移）
    NET_LOG_INFOF("App", "printf log: uid=%d, ratio=%.2f", 1001, 0.75);
    NET_LOG_WARNF("App", "low disk: %d MB left", 128);

    // 4) 直接调用单例（与 NetSdk::setLogLevel/enableFileLog 等价）
    Logger::getInstance().info("App", "direct singleton call");

    // 5) 多线程并发写日志：SDK 内部对控制台/文件分别加锁，不会串行
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([t]() {
            for (int i = 0; i < 20; ++i) {
                NET_LOG_I("Worker") << "thread=" << t << ", seq=" << i;
            }
        });
    }
    for (auto& th : workers) th.join();

    // 6) 级别过滤：低于当前级别的日志不会做任何字符串拼接
    Logger::getInstance().setLevel(LogLevel::Warn);
    NET_LOG_D("App") << "filtered out (will not appear)";
    NET_LOG_W("App") << "warning still goes to app.log";

    NET_LOG_FATALF("App", "fatal sample: code=%d", -1);
    Logger::getInstance().setLevel(LogLevel::Info);
    return 0;
}
