// 信号槽：连接、发射、参数类型容错（const char* -> std::string）、断开。
#include "test_util.h"
#include <net_sdk/SignalSlot.h>
#include <atomic>

int main() {
    using namespace net_sdk;
    auto& ss = SignalSlot::getInstance();

    std::atomic<int> hits{0};
    std::string got;

    int c1 = ss.connect("sig_int", [&](int v) { if (v == 7) hits++; });
    ss.emit("sig_int", 7);
    testutil::check(hits.load() == 1, "signal: int delivered");

    // const char* 字面量 -> std::string 槽：应安全转换而非抛 bad_any_cast 崩溃
    int c2 = ss.connect("sig_str", [&](const std::string& s) { got = s; });
    ss.emit("sig_str", "hello");
    testutil::eq(got, "hello", "signal: const char* to string");

    ss.disconnect(c1);
    ss.emit("sig_int", 7);
    testutil::check(hits.load() == 1, "signal: disconnect works");

    ss.disconnect(c2);
    return testutil::finish("signalslot");
}
