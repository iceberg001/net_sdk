/**
 * @file test_https.cpp
 * @brief HTTPS 集成测试：验证 SDK 在真实 TLS 连接下的可用性
 *
 * 覆盖：
 *  - 网络连通性探测（HTTP），无外网时跳过，避免 CI 误报
 *  - 默认配置下访问 https（libhv 自动创建默认 SSL_CTX）
 *  - configureSsl(verify=false) 后访问 https
 *  - HTTPS 返回结构完整（statusCode/body）
 */
#include "net_sdk/NetSdk.h"
#include <iostream>
#include <string>

using namespace net_sdk;

static void printResult(const char* tag, const NetResult& r) {
    std::cout << "[" << tag << "] code=" << static_cast<int>(r.code)
              << " status=" << r.statusCode
              << " bodyLen=" << r.body.size()
              << " msg=" << r.message << std::endl;
}

int main() {
    auto& sdk = NetSdk::getInstance();
    sdk.init();
    sdk.setLogLevel(LogLevel::Warn);
    auto& http = sdk.getHttpClient();

    const std::string httpUrl = "http://www.baidu.com/";
    const std::string httpsUrl = "https://www.baidu.com/";

    // 先探测网络：HTTP 都不通说明是环境问题，跳过而不是判定 HTTPS 失败。
    NetResult probe = http.getSync(httpUrl);
    printResult("probe-http", probe);
    if (!probe.ok()) {
        std::cout << "HTTPS test SKIPPED: no network connectivity" << std::endl;
        sdk.shutdown();
        return 0;
    }

    // 1) 默认配置（不显式 configureSsl），libhv 自动建立默认 SSL_CTX。
    NetResult r1 = http.getSync(httpsUrl);
    printResult("https-default", r1);

    // 2) 显式关闭校验。
    http.configureSsl(false);
    NetResult r2 = http.getSync(httpsUrl);
    printResult("https-noverify", r2);

    bool ok = (r1.ok() && r1.statusCode > 0) || (r2.ok() && r2.statusCode > 0);

    sdk.shutdown();
    if (!ok) {
        std::cerr << "HTTPS test FAILED: no successful TLS request" << std::endl;
        return 1;
    }
    std::cout << "HTTPS test PASSED" << std::endl;
    return 0;
}
