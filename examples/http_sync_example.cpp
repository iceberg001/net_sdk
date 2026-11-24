#include "net_sdk/NetSdk.h"
#include <iostream>

int main() {
    auto& sdk = net_sdk::NetSdk::getInstance();
    sdk.init();
    sdk.setLogLevel(net_sdk::LogLevel::Debug);
    sdk.enableFileLog(true, "app");  // 生成 YYYY-MM-DD_app.log，超过 5MB 依次轮转为 app1.log、app2.log ...
    sdk.enableConsoleLog(true);

    std::cout << "=== HTTP Sync Example ===" << std::endl;

    auto result = sdk.getHttpClient().getSync("http://httpbin.org/get");
    if (result.ok()) {
        std::cout << "Status: " << result.statusCode << std::endl;
        std::cout << "Body: " << result.body.substr(0, 500) << std::endl;
    } else {
        std::cout << "Request failed: " << result.message << std::endl;
    }

    auto postResult = sdk.getHttpClient().postSync("http://httpbin.org/post",
        R"({"key":"value"})", "application/json");
    if (postResult.ok()) {
        std::cout << "POST Status: " << postResult.statusCode << std::endl;
        std::cout << "POST Body: " << postResult.body.substr(0, 500) << std::endl;
    }

    sdk.shutdown();
    return 0;
}