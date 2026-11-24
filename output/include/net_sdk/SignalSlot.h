/**
 * @file SignalSlot.h
 * @brief NetSDK 信号槽系统
 *
 * 类似 Qt 信号槽机制的观察者模式实现，支持多模块订阅同一信号，
 * 当信号发射时所有订阅者都会收到通知。典型场景：账号切换后各业务模块自动处理。
 *
 * 典型用法：
 * @code
 *   // 订阅信号
 *   int connId = NET_CONNECT("account_changed", [](const std::string& newAccount) {
 *       std::cout << "Account changed to: " << newAccount << std::endl;
 *   });
 *
 *   // 发射信号（所有订阅者都会收到）
 *   NET_SIGNAL("account_changed", std::string("user_12345"));
 *
 *   // 断开订阅
 *   NET_DISCONNECT(connId);
 * @endcode
 *
 * @author NetSDK Team
 * @version 1.0.0
 * @date 2026-11-18
 */

#pragma once

#include <string>
#include <functional>
#include <map>
#include <vector>
#include <mutex>
#include <atomic>
#include <any>
#include <memory>
#include <type_traits>
#include <utility>
#include <tuple>

namespace net_sdk {

namespace detail {

// 从 lambda / 函数对象 / 函数指针中提取可调用签名，用于推导 connect 的参数类型。
template<typename T>
struct function_traits : function_traits<decltype(&T::operator())> {};

template<typename R, typename C, typename... Args>
struct function_traits<R(C::*)(Args...) const> {
    using void_function = std::function<void(Args...)>;
};

template<typename R, typename C, typename... Args>
struct function_traits<R(C::*)(Args...)> {
    using void_function = std::function<void(Args...)>;
};

template<typename R, typename... Args>
struct function_traits<R(*)(Args...)> {
    using void_function = std::function<void(Args...)>;
};

template<typename R, typename... Args>
struct function_traits<std::function<R(Args...)>> {
    using void_function = std::function<void(Args...)>;
};

} // namespace detail

/**
 * @brief 信号槽管理器（单例）
 *
 * 提供信号订阅（connect）、信号发射（emit）、断开订阅（disconnect）功能。
 * 支持任意参数类型的信号，通过 std::any 实现类型擦除。
 * 线程安全，所有操作均通过互斥锁保护。
 */
class SignalSlot {
public:
    /// 获取 SignalSlot 单例实例
    static SignalSlot& getInstance();

    SignalSlot();
    ~SignalSlot();

    SignalSlot(const SignalSlot&) = delete;
    SignalSlot& operator=(const SignalSlot&) = delete;

    /**
     * @brief 订阅信号（使用 std::function）
     * @tparam Args 信号参数类型列表
     * @param signalName 信号名称
     * @param slot 槽函数（回调）
     * @return 连接 ID，用于后续断开连接
     */
    template<typename... Args>
    int connect(const std::string& signalName, std::function<void(Args...)> slot) {
        std::lock_guard<std::mutex> lock(mutex_);
        int connId = nextConnId_++;
        auto& slots = slots_[signalName];
        slots.push_back({connId, [slot](const std::vector<std::any>& args) {
            if constexpr (sizeof...(Args) == 0) {
                slot();
            } else {
                invokeSlot<Args...>(slot, args, std::index_sequence_for<Args...>{});
            }
        }});
        return connId;
    }

    /**
     * @brief 订阅信号（使用普通函数指针）
     * @tparam Args 信号参数类型列表
     * @param signalName 信号名称
     * @param slot 函数指针
     * @return 连接 ID
     */
    template<typename... Args>
    int connect(const std::string& signalName, void(*slot)(Args...)) {
        return connect<Args...>(signalName, std::function<void(Args...)>(slot));
    }

    /**
     * @brief 订阅信号（使用 lambda 或仿函数）
     * @tparam Lambda lambda 类型
     * @tparam Args 信号参数类型列表
     * @param signalName 信号名称
     * @param lambda lambda 表达式
     * @return 连接 ID
     */
    template<typename Lambda>
    int connect(const std::string& signalName, Lambda&& lambda) {
        using VoidFunc = typename detail::function_traits<std::decay_t<Lambda>>::void_function;
        return connect(signalName, VoidFunc(std::forward<Lambda>(lambda)));
    }

    /**
     * @brief 断开指定连接
     * @param connId 连接 ID（由 connect 返回）
     */
    void disconnect(int connId);

    /**
     * @brief 断开信号的所有连接
     * @param signalName 信号名称
     */
    void disconnectAll(const std::string& signalName);

    /**
     * @brief 发射信号，通知所有订阅者
     * @tparam Args 信号参数类型列表
     * @param signalName 信号名称
     * @param args 信号参数
     */
    template<typename... Args>
    void emit(const std::string& signalName, Args... args) {
        std::vector<std::function<void(const std::vector<std::any>&)>> slotsCopy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = slots_.find(signalName);
            if (it == slots_.end()) return;
            for (auto& entry : it->second) {
                slotsCopy.push_back(entry.slot);
            }
        }
        std::vector<std::any> packedArgs = {std::any(args)...};
        for (auto& slot : slotsCopy) {
            slot(packedArgs);
        }
    }

    /// 检查指定信号是否存在订阅者
    bool hasSignal(const std::string& signalName) const;

    /// 获取指定信号的订阅者数量
    int getConnectionCount(const std::string& signalName) const;

private:
    // 单个参数的安全转换：精确匹配直接用；const char*/char* 可适配 std::string。
    // 返回 false 表示该参数类型不兼容，此时跳过该槽而不是抛异常终止发射线程。
    template<typename T>
    static bool tryGetArg(const std::any& a, T& out) {
        if (a.type() == typeid(T)) {
            out = std::any_cast<T>(a);
            return true;
        }
        return false;
    }

    static bool tryGetArg(const std::any& a, std::string& out) {
        if (a.type() == typeid(std::string)) {
            out = std::any_cast<std::string>(a);
            return true;
        }
        if (a.type() == typeid(const char*)) {
            out = std::any_cast<const char*>(a);
            return true;
        }
        if (a.type() == typeid(char*)) {
            out = std::any_cast<char*>(a);
            return true;
        }
        return false;
    }

    template<typename... Args, std::size_t... Is>
    static void invokeSlot(std::function<void(Args...)> slot, const std::vector<std::any>& args, std::index_sequence<Is...>) {
        // 槽参数可能是 const T& / T&，统一 decay 为值类型进行转换与承载，
        // 否则回退路径中 std::get 返回引用会匹配错误的 tryGetArg 重载。
        using Decayed = std::tuple<std::decay_t<Args>...>;
        try {
            // 精确匹配路径：覆盖所有原本 any_cast 能成功的场景。
            slot(std::any_cast<std::tuple_element_t<Is, std::tuple<Args...>>>(args[Is])...);
        } catch (const std::bad_any_cast&) {
            // 类型不完全匹配时，逐参数尝试宽松转换（如字符串字面量 -> std::string）。
            // 仅在所有参数都可默认构造时才走此回退路径，避免对不可默认构造类型编译失败。
            if constexpr ((std::is_default_constructible_v<std::decay_t<Args>> && ...)) {
                try {
                    Decayed params{};
                    bool ok = true;
                    ((ok = ok && tryGetArg(args[Is], std::get<Is>(params))), ...);
                    if (ok) std::apply(slot, std::move(params));
                } catch (...) {
                    // 参数无法构造或槽执行抛异常：吞掉以保护发射方线程。
                }
            }
        } catch (...) {
            // 槽函数自身抛出的异常：吞掉，避免跨线程传播导致终止。
        }
    }

    struct SlotEntry {
        int connId;
        std::function<void(const std::vector<std::any>&)> slot;
    };

    std::map<std::string, std::vector<SlotEntry>> slots_;
    mutable std::mutex mutex_;
    std::atomic<int> nextConnId_{1};
};

/// @brief 发射信号宏
#define NET_SIGNAL(name, ...) net_sdk::SignalSlot::getInstance().emit(name, ##__VA_ARGS__)
/// @brief 订阅信号宏
#define NET_CONNECT(signal, slot) net_sdk::SignalSlot::getInstance().connect(signal, slot)
/// @brief 断开订阅宏
#define NET_DISCONNECT(connId) net_sdk::SignalSlot::getInstance().disconnect(connId)

} // namespace net_sdk