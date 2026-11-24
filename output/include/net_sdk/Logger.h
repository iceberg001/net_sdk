/**
 * @file Logger.h
 * @brief NetSDK 日志系统
 *
 * 提供线程安全的日志输出功能，支持控制台彩色输出和文件输出，
 * 支持日志级别过滤、文件自动轮转、多级别日志（Trace/Debug/Info/Warn/Error/Fatal）。
 *
 * 使用方式：
 * @code
 *   // 1) 经典标签 + 字符串
 *   NET_LOG_INFO("MyModule", "This is an info message");
 *
 *   // 2) 流式（任意 ostream 可输出类型，最贴近业务日志习惯）
 *   NET_LOG_I("MyModule") << "uid=" << 1001 << ", name=" << name;
 *
 *   // 3) printf 风格
 *   NET_LOG_INFOF("MyModule", "uid=%d, name=%s", 1001, name.c_str());
 *
 *   // 4) 一行完成配置（其它工程推荐）
 *   net_sdk::configureLogging(net_sdk::LogLevel::Info, false, "app");
 * @endcode
 *
 * 说明：本头文件只依赖 C++11 标准库，不依赖 SDK 的其它公共头，
 * 因此以 /std:c++14 甚至 C++11 编译的既有工程也可以直接 include 使用，
 * 只需在链接时加入 net_sdk.lib（或 libnet_sdk.a）即可。
 *
 * @author NetSDK Team
 * @version 1.0.0
 * @date 2026-11-18
 */

#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <atomic>
#include <memory>
#include <chrono>
#include <sstream>
#include <utility>
#include <cstdarg>
#include <cstdio>

namespace net_sdk {

/**
 * @brief 日志级别枚举
 *
 * 从 Trace（最详细）到 Fatal（最严重）共 6 个级别，
 * 设置某个级别后，只有等于或高于该级别的日志才会被输出。
 */
enum class LogLevel {
    Trace = 0,  ///< 最详细的跟踪信息，通常仅在开发调试时启用
    Debug = 1,  ///< 调试信息，用于开发阶段问题定位
    Info = 2,   ///< 一般信息，记录程序正常运行状态
    Warn = 3,   ///< 警告信息，表示可能存在潜在问题
    Error = 4,  ///< 错误信息，表示操作失败但程序可继续运行
    Fatal = 5   ///< 致命错误，表示程序无法继续运行
};

/**
 * @brief 日志系统单例类
 *
 * 线程安全的日志管理器，支持控制台和文件两种输出方式。
 * 文件输出支持自动轮转，当文件超过指定大小时自动创建新文件。
 *
 * 典型用法：
 * @code
 *   Logger::getInstance().setLevel(LogLevel::Debug);
 *   Logger::getInstance().setFileOutput(true, "myapp.log");
 *   NET_LOG_INFO("ModuleName", "Log message here");
 * @endcode
 */
class Logger {
public:
    /// 获取 Logger 单例实例
    static Logger& getInstance();

    /// 设置日志输出级别，低于此级别的日志将被忽略
    void setLevel(LogLevel level);

    /// 获取当前日志输出级别
    LogLevel getLevel() const;

    /// 启用或禁用控制台输出（默认启用）
    void setConsoleOutput(bool enable);

    /**
     * @brief 启用或禁用文件输出
     *
     * 传入日志"基础名"（如 "app"），SDK 会自动生成带日期前缀的文件名：
     *   - 首个文件：  2026-11-18_app.log
     *   - 超过大小后：2026-11-18_app1.log
     *   - 继续轮转：  2026-11-18_app2.log ... 依次类推
     * 日期为启用日志文件当天的日期，进程生命周期内保持不变。
     * 可带目录，如 "logs/app" → "logs/2026-11-18_app.log"（目录会自动创建）；
     * 也可带扩展名，如 "app.log"，扩展名会被忽略。
     *
     * @param enable 是否启用文件输出
     * @param filePath 日志文件基础名或路径（为空则使用默认名 "net_sdk"）
     */
    void setFileOutput(bool enable, const std::string& filePath = "");

    /// 设置单个日志文件最大大小（字节），超过后自动轮转（默认 5MB）
    void setMaxFileSize(size_t maxSize);

    /// 设置保留的日志文件最大数量（默认 5 个）
    void setMaxFileCount(int count);

    /**
     * @brief 输出一条日志
     * @param level 日志级别
     * @param tag 日志标签（通常为模块名）
     * @param message 日志内容
     */
    void log(LogLevel level, const std::string& tag, const std::string& message);

    void trace(const std::string& tag, const std::string& message); ///< 输出 Trace 级别日志
    void debug(const std::string& tag, const std::string& message); ///< 输出 Debug 级别日志
    void info(const std::string& tag, const std::string& message);  ///< 输出 Info 级别日志
    void warn(const std::string& tag, const std::string& message);  ///< 输出 Warn 级别日志
    void error(const std::string& tag, const std::string& message); ///< 输出 Error 级别日志
    void fatal(const std::string& tag, const std::string& message); ///< 输出 Fatal 级别日志

    /// 将日志级别转换为字符串表示
    static const char* levelToString(LogLevel level);

private:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void rotateFileIfNeeded();
    void writeToFile(const std::string& formattedMsg);
    std::string formatMessage(LogLevel level, const std::string& tag, const std::string& message);

    // 由基础名生成带日期前缀的文件路径；index=0 为首个文件，>0 追加序号。
    std::string makeLogFilePath(int index) const;
    // 从用户传入的路径/基础名中解析出目录、基础名，并计算当天日期前缀。
    void configureLogPath(const std::string& input);

    std::atomic<LogLevel> level_{LogLevel::Info};
    std::atomic<bool> consoleOutput_{true};
    std::atomic<bool> fileOutput_{false};
    std::string logDirectory_;   // 目录（含末尾斜杠），可为空
    std::string logBaseName_{"net_sdk"}; // 不含日期、序号、扩展名的基础名
    std::string datePrefix_;     // "YYYY-MM-DD"，启用时计算一次
    int currentFileIndex_{0};    // 当前写入的文件序号：0 → app.log，1 → app1.log ...
    std::string filePath_{"net_sdk.log"}; // 当前完整文件路径（= makeLogFilePath(currentFileIndex_)）
    std::atomic<size_t> maxFileSize_{5 * 1024 * 1024};
    std::atomic<int> maxFileCount_{5};
    std::mutex fileMutex_;
    std::mutex consoleMutex_;
    std::unique_ptr<std::ofstream> fileStream_;
};

/// @brief Trace 级别日志宏
#define NET_LOG_TRACE(tag, msg) net_sdk::Logger::getInstance().trace(tag, msg)
/// @brief Debug 级别日志宏
#define NET_LOG_DEBUG(tag, msg) net_sdk::Logger::getInstance().debug(tag, msg)
/// @brief Info 级别日志宏
#define NET_LOG_INFO(tag, msg)  net_sdk::Logger::getInstance().info(tag, msg)
/// @brief Warn 级别日志宏
#define NET_LOG_WARN(tag, msg)  net_sdk::Logger::getInstance().warn(tag, msg)
/// @brief Error 级别日志宏
#define NET_LOG_ERROR(tag, msg) net_sdk::Logger::getInstance().error(tag, msg)
/// @brief Fatal 级别日志宏
#define NET_LOG_FATAL(tag, msg) net_sdk::Logger::getInstance().fatal(tag, msg)

// ============================================================================
//  面向外部工程的便捷日志 API
//
//  全部为头文件内的 inline 实现，不新增库符号、不改变 Logger 类 ABI，
//  因此其它工程只要 include 本头并链接 net_sdk 库即可直接使用。
// ============================================================================

namespace detail {

/// printf 风格格式化（每次使用独立缓冲区，线程安全）
inline std::string formatLogV(const char* fmt, va_list args) {
    if (!fmt) return std::string();
    va_list copy;
    va_copy(copy, args);
    const int n = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    if (n < 0) return std::string(fmt);
    std::string buf(static_cast<size_t>(n) + 1, '\0');
    std::vsnprintf(&buf[0], buf.size(), fmt, args);
    buf.resize(static_cast<size_t>(n));
    return buf;
}

} // namespace detail

/// va_list 版本的格式化日志（供二次封装使用）
inline void logfv(LogLevel level, const std::string& tag, const char* fmt, va_list args) {
    Logger::getInstance().log(level, tag, detail::formatLogV(fmt, args));
}

/// printf 风格日志函数：net_sdk::infof("Tag", "uid=%d name=%s", id, name.c_str());
inline void tracef(const std::string& tag, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); logfv(LogLevel::Trace, tag, fmt, ap); va_end(ap);
}
inline void debugf(const std::string& tag, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); logfv(LogLevel::Debug, tag, fmt, ap); va_end(ap);
}
inline void infof(const std::string& tag, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); logfv(LogLevel::Info, tag, fmt, ap); va_end(ap);
}
inline void warnf(const std::string& tag, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); logfv(LogLevel::Warn, tag, fmt, ap); va_end(ap);
}
inline void errorf(const std::string& tag, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); logfv(LogLevel::Error, tag, fmt, ap); va_end(ap);
}
inline void fatalf(const std::string& tag, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); logfv(LogLevel::Fatal, tag, fmt, ap); va_end(ap);
}

/**
 * @brief 流式日志对象（RAII：表达式结束时输出一行）
 *
 * 用法：NET_LOG_I("Tag") << "uid=" << 1001 << ", ok=" << true;
 * 请直接在表达式中使用，不要保存 LogStream 对象（不可拷贝）。
 * 低于当前日志级别时不会做任何字符串拼接，开销极小。
 */
class LogStream {
public:
    LogStream(LogLevel level, std::string tag)
        : level_(level), tag_(std::move(tag)),
          active_(Logger::getInstance().getLevel() <= level) {}

    ~LogStream() {
        if (active_) {
            Logger::getInstance().log(level_, tag_, oss_.str());
        }
    }

    LogStream(const LogStream&) = delete;
    LogStream& operator=(const LogStream&) = delete;

    template <typename T>
    LogStream& operator<<(const T& value) {
        if (active_) oss_ << value;
        return *this;
    }

    /// 支持 std::endl 等流操纵符
    LogStream& operator<<(std::ostream& (*manip)(std::ostream&)) {
        if (active_) oss_ << manip;
        return *this;
    }

private:
    LogLevel level_;
    std::string tag_;
    bool active_;
    std::ostringstream oss_;
};

/// 流式日志宏：NET_LOG_I("Tag") << "a=" << a << ", b=" << b;
#define NET_LOG_T(tag) net_sdk::LogStream(net_sdk::LogLevel::Trace, tag)
#define NET_LOG_D(tag) net_sdk::LogStream(net_sdk::LogLevel::Debug, tag)
#define NET_LOG_I(tag) net_sdk::LogStream(net_sdk::LogLevel::Info,  tag)
#define NET_LOG_W(tag) net_sdk::LogStream(net_sdk::LogLevel::Warn,  tag)
#define NET_LOG_E(tag) net_sdk::LogStream(net_sdk::LogLevel::Error, tag)
#define NET_LOG_F(tag) net_sdk::LogStream(net_sdk::LogLevel::Fatal, tag)

/// printf 风格日志宏：NET_LOG_INFOF("Tag", "uid=%d", id);
#define NET_LOG_TRACEF(tag, ...) net_sdk::tracef(tag, __VA_ARGS__)
#define NET_LOG_DEBUGF(tag, ...) net_sdk::debugf(tag, __VA_ARGS__)
#define NET_LOG_INFOF(tag, ...)  net_sdk::infof(tag,  __VA_ARGS__)
#define NET_LOG_WARNF(tag, ...)  net_sdk::warnf(tag,  __VA_ARGS__)
#define NET_LOG_ERRORF(tag, ...) net_sdk::errorf(tag, __VA_ARGS__)
#define NET_LOG_FATALF(tag, ...) net_sdk::fatalf(tag, __VA_ARGS__)

/**
 * @brief 一行完成日志配置（外部工程推荐入口）
 *
 * @param level 日志级别
 * @param console 是否输出到控制台
 * @param fileBaseName 文件日志基础名，非空即启用文件输出，
 *                     例如 "app" → YYYY-MM-DD_app.log，超过 5MB 轮转为 app1.log ...
 *                     为空则不写文件
 */
inline void configureLogging(LogLevel level = LogLevel::Info,
                             bool console = true,
                             const std::string& fileBaseName = "") {
    Logger& logger = Logger::getInstance();
    logger.setLevel(level);
    logger.setConsoleOutput(console);
    logger.setFileOutput(!fileBaseName.empty(), fileBaseName);
}

/// 调整文件轮转参数（单个文件最大字节数、最多保留文件个数）
inline void configureLogRotation(size_t maxFileSize, int maxFileCount) {
    Logger::getInstance().setMaxFileSize(maxFileSize);
    Logger::getInstance().setMaxFileCount(maxFileCount);
}

} // namespace net_sdk