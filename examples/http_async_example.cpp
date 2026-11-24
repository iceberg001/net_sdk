#include "net_sdk/NetSdk.h"
#include <iostream>
#include <chrono>
#include <thread>

int main() {
    auto& sdk = net_sdk::NetSdk::getInstance();
    sdk.init();
    sdk.setLogLevel(net_sdk::LogLevel::Debug);
    sdk.enableFileLog(true, "app");  // 生成 YYYY-MM-DD_app.log，超过 5MB 依次轮转为 app1.log、app2.log ...

    std::cout << "=== HTTP Async Example ===" << std::endl;

    int completedCount = 0;
    std::mutex mtx;
    std::condition_variable cv;

    sdk.getHttpClient().getAsync("http://httpbin.org/get", [&](const net_sdk::NetResult& result) {
        std::lock_guard<std::mutex> lock(mtx);
        std::cout << "Async GET completed - Status: " << result.statusCode << std::endl;
        std::cout << "Body: " << result.body.substr(0, 300) << std::endl;
        completedCount++;
        cv.notify_one();
    });

    sdk.getHttpClient().postAsync("http://httpbin.org/post",
        R"({"name":"net_sdk","version":"1.0"})",
        [&](const net_sdk::NetResult& result) {
            std::lock_guard<std::mutex> lock(mtx);
            std::cout << "Async POST completed - Status: " << result.statusCode << std::endl;
            completedCount++;
            cv.notify_one();
        });

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&] { return completedCount >= 2; });
    }

    std::cout << "All async requests completed" << std::endl;
    sdk.shutdown();
    return 0;
}