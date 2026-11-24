#include "net_sdk/SignalSlot.h"
#include "net_sdk/Logger.h"

namespace net_sdk {

SignalSlot& SignalSlot::getInstance() {
    // 进程生命周期常驻，避免退出阶段与其它单例的析构顺序 UAF。
    static SignalSlot* instance = new SignalSlot();
    return *instance;
}

SignalSlot::SignalSlot() {
    NET_LOG_DEBUG("SignalSlot", "SignalSlot system initialized");
}

SignalSlot::~SignalSlot() {
    std::lock_guard<std::mutex> lock(mutex_);
    slots_.clear();
}

void SignalSlot::disconnect(int connId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [name, slotList] : slots_) {
        for (auto it = slotList.begin(); it != slotList.end(); ++it) {
            if (it->connId == connId) {
                slotList.erase(it);
                NET_LOG_DEBUG("SignalSlot", "Disconnected connId=" + std::to_string(connId) + " from signal=" + name);
                return;
            }
        }
    }
}

void SignalSlot::disconnectAll(const std::string& signalName) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = slots_.find(signalName);
    if (it != slots_.end()) {
        int count = static_cast<int>(it->second.size());
        it->second.clear();
        NET_LOG_DEBUG("SignalSlot", "Disconnected all " + std::to_string(count) + " slots from signal=" + signalName);
    }
}

bool SignalSlot::hasSignal(const std::string& signalName) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slots_.find(signalName) != slots_.end();
}

int SignalSlot::getConnectionCount(const std::string& signalName) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = slots_.find(signalName);
    if (it == slots_.end()) return 0;
    return static_cast<int>(it->second.size());
}

} // namespace net_sdk