#ifndef _MYLOG_H
#define _MYLOG_H

#include <fstream>
#include <mutex>
#include <string>
#include <sstream>

// 日志级别（数值越大越严重，便于过滤）
// 注意：不能直接用 DEBUG/INFO/WARN/ERROR 作为枚举值，
// 会与 windows.h 里的宏（如 wingdi.h 的 ERROR）冲突
enum Priority
{
    PRIO_DEBUG = 0,
    PRIO_INFO  = 1,
    PRIO_WARN  = 2,
    PRIO_ERROR = 3
};

class LogCpp
{
public:
    static LogCpp& getInstance();
    static void destroy();

    // 初始化：输出目录、文件名前缀（默认 "../log" 和 "log"）
    // 必须在首次写日志前调用；重复调用会关闭旧文件并按新参数重建
    void init(const std::string& dir = "../log",
              const std::string& prefix = "log");

    void setPriority(Priority priority);

    void error(const char* msg);
    void warn(const char* msg);
    void info(const char* msg);
    void debug(const char* msg);

    // 供宏使用：带调用位置的日志入口
    void logWithSite(Priority level, const char* msg,
                     const char* file, const char* func, int line);

private:
    LogCpp();
    ~LogCpp();
    LogCpp(const LogCpp&) = delete;
    LogCpp& operator=(const LogCpp&) = delete;

    void write(Priority level, const char* msg,
               const char* file, const char* func, int line);

    static const char* levelName(Priority level);

    // 文件管理
    void ensureFileForToday();          // 确保当前日期/大小对应文件已打开
    void openNextFile();                // 关闭当前，打开下一个序号
    std::string buildFileName(int index) const;
    static std::string currentDateStr(); // YYYYMMDD

private:
    static LogCpp* plog_;

    std::mutex    mutex_;
    Priority      minLevel_;
    std::ofstream fileOut_;

    // 文件配置
    std::string   dir_;
    std::string   prefix_;
    std::string   curDate_;   // 当前文件对应的日期 YYYYMMDD
    int           curIndex_;  // 当前滚动序号，0 表示无后缀
    uint64_t      maxSize_;   // 单文件大小上限，默认 5MB
};

// ---------------------------------------------------------------------------
// 宏：调用处捕获 file/func/line
// 注意：故意命名为 APPLOG_*，不能叫 LOG_ERROR/LOG_INFO ——
//       libobs 的 obs.h 里已定义 LOG_ERROR(100)/LOG_WARNING(200)/
//       LOG_INFO(300)/LOG_DEBUG(400) 常量宏，供 blog() 使用，同名会冲突
// ---------------------------------------------------------------------------
#define APPLOG_ERROR(msg)                                               \
    LogCpp::getInstance().logWithSite(PRIO_ERROR, msg, __FILE__, __func__, __LINE__)

#define APPLOG_WARN(msg)                                                \
    LogCpp::getInstance().logWithSite(PRIO_WARN, msg, __FILE__, __func__, __LINE__)

#define APPLOG_INFO(msg)                                                \
    LogCpp::getInstance().logWithSite(PRIO_INFO, msg, __FILE__, __func__, __LINE__)

#define APPLOG_DEBUG(msg)                                               \
    LogCpp::getInstance().logWithSite(PRIO_DEBUG, msg, __FILE__, __func__, __LINE__)

#endif // _MYLOG_H