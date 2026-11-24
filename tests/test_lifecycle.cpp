// 生命周期：未初始化访问器不崩溃、参数校验（loopCount=0）、init/shutdown 幂等与并发。
#include "test_util.h"
#include <net_sdk/NetSdk.h>
#include <thread>
#include <atomic>

int main() {
    using namespace net_sdk;
    auto& sdk = NetSdk::getInstance();

    // 未 init 直接访问各访问器：应隐式初始化，不返回空引用导致崩溃
    bool noCrash = true;
    try {
        auto& h = sdk.getHttpClient();
        auto& c = sdk.getCommandRequest();
        auto& t = sdk.getThreadPool();
        auto& e = sdk.getEventLoopPool();
        (void)h; (void)c; (void)t; (void)e;
    } catch (...) {
        noCrash = false;
    }
    testutil::check(noCrash, "lifecycle: accessors safe before init");
    testutil::check(sdk.isInitialized(), "lifecycle: implicit init happened");

    // 幂等 init
    testutil::check(sdk.init(2, 2), "lifecycle: init idempotent true");

    // 非法参数被夹紧为 1，不除零崩溃
    sdk.shutdown();
    bool paramOk = true;
    try {
        sdk.init(0, 0);
    } catch (...) {
        paramOk = false;
    }
    testutil::check(paramOk, "lifecycle: invalid params clamped");

    // 并发 init/shutdown 不崩溃（加锁保护）
    sdk.shutdown();
    std::atomic<bool> crash{false};
    std::thread a([&]() { try { for (int i = 0; i < 20; ++i) NetSdk::getInstance().init(2, 2); } catch (...) { crash = true; } });
    std::thread b([&]() { try { for (int i = 0; i < 20; ++i) NetSdk::getInstance().shutdown(); } catch (...) { crash = true; } });
    a.join(); b.join();
    testutil::check(!crash.load(), "lifecycle: concurrent init/shutdown safe");

    sdk.shutdown();
    return testutil::finish("lifecycle");
}
