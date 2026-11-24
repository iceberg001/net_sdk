#include "net_sdk/Logger.h"
#include <iostream>
#include <iomanip>
#include <ctime>
#include <sstream>
#include <cstdio>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#include <sys/types.h>
#endif

namespace net_sdk {

Logger& Logger::getInstance() {
    // 故意使用进程生命周期常驻的堆对象（不析构）：
    // 其它单例（NetSdk/CommandRequest 等）在程序退出阶段析构时会调用 NET_LOG_*，
    // 若 Logger 先于它们析构将触发 use-after-free。日志器常驻可彻底避免析构顺序问题。
    static Logger* instance = new Logger();
    return *instance;
}

Logger::Logger() {
    fileStream_ = std::make_unique<std::ofstream>();
}

Logger::~Logger() {
    if (fileStream_ && fileStream_->is_open()) {
        fileStream_->close();
    }
}

void Logger::setLevel(LogLevel level) {
    level_.store(level);
}

LogLevel Logger::getLevel() const {
    return level_.load();
}

void Logger::setConsoleOutput(bool enable) {
    consoleOutput_.store(enable);
}

void Logger::configureLogPath(const std::string& input) {
    // 解析出目录与基础名：支持 "app"、"logs/app"、"logs\\app.log" 等形式。
    std::string name = input;
    std::string::size_type slash = name.find_last_of("/\\");
    if (slash != std::string::npos) {
        logDirectory_ = name.substr(0, slash + 1);
        name = name.substr(slash + 1);
    } else {
        logDirectory_.clear();
    }

    // 去掉扩展名（.log / .txt 等一律忽略，由 SDK 统一使用 .log）。
    std::string::size_type dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        name = name.substr(0, dot);
    }
    logBaseName_ = name.empty() ? "net_sdk" : name;

    // 计算当天日期前缀，进程生命周期内固定，保证同一批轮转文件日期一致。
    std::time_t now = std::time(nullptr);
    std::tm tmVal{};
#ifdef _WIN32
    localtime_s(&tmVal, &now);
#else
    localtime_r(&now, &tmVal);
#endif
    char dateBuf[16] = {0};
    std::strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", &tmVal);
    datePrefix_ = dateBuf;

    // 确保目录存在（支持多级目录，Windows 与 POSIX 均可）。
    if (!logDirectory_.empty()) {
        std::string accumulated;
        for (std::string::size_type i = 0; i < logDirectory_.size(); ++i) {
            char ch = logDirectory_[i];
            if (ch == '/' || ch == '\\') {
                if (!accumulated.empty()) {
#ifdef _WIN32
                    _mkdir(accumulated.c_str());
#else
                    mkdir(accumulated.c_str(), 0755);
#endif
                }
            }
            accumulated.push_back(ch);
        }
    }

    currentFileIndex_ = 0;
}

std::string Logger::makeLogFilePath(int index) const {
    std::string path = logDirectory_ + datePrefix_ + "_" + logBaseName_;
    if (index > 0) {
        path += std::to_string(index);
    }
    path += ".log";
    return path;
}

void Logger::setFileOutput(bool enable, const std::string& filePath) {
    std::lock_guard<std::mutex> lock(fileMutex_);
    if (fileStream_ && fileStream_->is_open()) {
        fileStream_->close();
    }

    if (enable) {
        // 传入了新路径（或首次启用）时重新计算日期前缀与基础名；
        // 仅切换开关（ filePath 为空）时沿用上次配置。
        if (!filePath.empty()) {
            configureLogPath(filePath);
        } else if (datePrefix_.empty()) {
            configureLogPath("net_sdk");
        }
        filePath_ = makeLogFilePath(currentFileIndex_);
        fileStream_->open(filePath_, std::ios::app);
    }

    fileOutput_.store(enable);
}

void Logger::setMaxFileSize(size_t maxSize) {
    maxFileSize_.store(maxSize);
}

void Logger::setMaxFileCount(int count) {
    maxFileCount_.store(count);
}

const char* Logger::levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        default: return "UNKNOWN";
    }
}

std::string Logger::formatMessage(LogLevel level, const std::string& tag, const std::string& message) {
    auto now = std::chrono::system_clock::now();
    auto timeT = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tmVal;
#ifdef _WIN32
    localtime_s(&tmVal, &timeT);
#else
    localtime_r(&timeT, &tmVal);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tmVal, "%Y-%m-%d %H:%M:%S");
    oss << "." << std::setfill('0') << std::setw(3) << ms.count();
    oss << " [" << levelToString(level) << "]";
    oss << " [" << tag << "]";
    oss << " " << message;
    return oss.str();
}

// 调用方（writeToFile）已持有 fileMutex_，此处不得再次加锁，否则自死锁。
// 轮转策略：app.log → app1.log → app2.log ... 序号递增，靠后的文件更新。
void Logger::rotateFileIfNeeded() {
    if (!fileStream_ || !fileStream_->is_open()) return;

    fileStream_->close();

    const int nextIndex = currentFileIndex_ + 1;
    const int maxCount = maxFileCount_.load();

    // 超出保留数量时删除最老的一个文件（index = nextIndex - maxCount），
    // 使磁盘上始终最多保留 maxCount 个轮转文件，无需搬移重命名。
    if (maxCount > 0 && nextIndex >= maxCount) {
        std::remove(makeLogFilePath(nextIndex - maxCount).c_str());
    }

    currentFileIndex_ = nextIndex;
    filePath_ = makeLogFilePath(currentFileIndex_);
    fileStream_->open(filePath_, std::ios::app);
}

void Logger::writeToFile(const std::string& formattedMsg) {
    if (!fileOutput_.load()) return;

    std::lock_guard<std::mutex> lock(fileMutex_);
    if (!fileStream_ || !fileStream_->is_open()) {
        fileStream_->open(filePath_, std::ios::app);
        if (!fileStream_->is_open()) return;
    }

    *fileStream_ << formattedMsg << std::endl;
    fileStream_->flush();

    fileStream_->seekp(0, std::ios::end);
    std::streampos size = fileStream_->tellp();
    if (static_cast<size_t>(size) > maxFileSize_.load()) {
        rotateFileIfNeeded();
    }
}

void Logger::log(LogLevel level, const std::string& tag, const std::string& message) {
    if (level < level_.load()) return;

    std::string formatted = formatMessage(level, tag, message);

    if (consoleOutput_.load()) {
        // 控制台输出加锁，避免多线程日志行交错、颜色串位。
        std::lock_guard<std::mutex> consoleLock(consoleMutex_);
#ifdef _WIN32
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        WORD color = 7;
        switch (level) {
            case LogLevel::Trace: color = 8; break;
            case LogLevel::Debug: color = 3; break;
            case LogLevel::Info:  color = 7; break;
            case LogLevel::Warn:  color = 14; break;
            case LogLevel::Error: color = 12; break;
            case LogLevel::Fatal: color = 12 | 8; break;
        }
        SetConsoleTextAttribute(hConsole, color);
        std::cout << formatted << std::endl;
        SetConsoleTextAttribute(hConsole, 7);
#else
        const char* colorCode = "";
        switch (level) {
            case LogLevel::Trace: colorCode = "\033[90m"; break;
            case LogLevel::Debug: colorCode = "\033[36m"; break;
            case LogLevel::Info:  colorCode = "\033[0m"; break;
            case LogLevel::Warn:  colorCode = "\033[33m"; break;
            case LogLevel::Error: colorCode = "\033[31m"; break;
            case LogLevel::Fatal: colorCode = "\033[31m\033[1m"; break;
        }
        std::cout << colorCode << formatted << "\033[0m" << std::endl;
#endif
    }

    writeToFile(formatted);
}

void Logger::trace(const std::string& tag, const std::string& message) { log(LogLevel::Trace, tag, message); }
void Logger::debug(const std::string& tag, const std::string& message) { log(LogLevel::Debug, tag, message); }
void Logger::info(const std::string& tag, const std::string& message)  { log(LogLevel::Info, tag, message); }
void Logger::warn(const std::string& tag, const std::string& message)  { log(LogLevel::Warn, tag, message); }
void Logger::error(const std::string& tag, const std::string& message) { log(LogLevel::Error, tag, message); }
void Logger::fatal(const std::string& tag, const std::string& message) { log(LogLevel::Fatal, tag, message); }

} // namespace net_sdk