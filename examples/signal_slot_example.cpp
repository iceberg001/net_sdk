#include "net_sdk/NetSdk.h"
#include <iostream>
#include <string>

int main() {
    auto& sdk = net_sdk::NetSdk::getInstance();
    sdk.init();
    sdk.setLogLevel(net_sdk::LogLevel::Debug);
    sdk.enableFileLog(true, "app");  // 生成 YYYY-MM-DD_app.log，超过 5MB 依次轮转为 app1.log、app2.log ...

    std::cout << "=== Signal Slot Example ===" << std::endl;

    auto& signalSlot = sdk.getSignalSlot();

    int connId1 = NET_CONNECT("account_changed", [](const std::string& newAccount) {
        std::cout << "[Module A] Account changed to: " << newAccount << std::endl;
    });

    int connId2 = NET_CONNECT("account_changed", [](const std::string& newAccount) {
        std::cout << "[Module B] Account switched, reloading data for: " << newAccount << std::endl;
    });

    int connId3 = NET_CONNECT("account_changed", [](const std::string& newAccount) {
        std::cout << "[Module C] Clearing cache for old account, new: " << newAccount << std::endl;
    });

    std::cout << "Connected " << signalSlot.getConnectionCount("account_changed")
              << " slots to 'account_changed'" << std::endl;

    std::cout << "\n--- Emitting signal: account_changed ---" << std::endl;
    NET_SIGNAL("account_changed", std::string("user_12345"));

    std::cout << "\n--- Disconnect Module B ---" << std::endl;
    NET_DISCONNECT(connId2);

    std::cout << "\n--- Emitting signal again ---" << std::endl;
    NET_SIGNAL("account_changed", std::string("user_67890"));

    std::cout << "\n--- Custom signal with multiple params ---" << std::endl;
    NET_CONNECT("data_received", [](int cmdId, const std::string& data) {
        std::cout << "[Handler] cmdId=" << cmdId << " data=" << data << std::endl;
    });
    NET_SIGNAL("data_received", 1001, std::string("hello world"));

    sdk.shutdown();
    return 0;
}