#include "log_cpp.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#endif

// ---------------------------------------------------------------------------
// 单例
// ---------------------------------------------------------------------------
LogCpp* LogCpp::plog_ = nullptr;

LogCpp& LogCpp::getInstance()
{
    if (plog_ == nullptr)
    {
        plog_ = new LogCpp;
    }
    return *plog_;
}

void LogCpp::destroy()
{
    if (plog_)
    {
        plog_->write(PRIO_INFO, "LogCpp destroy", __FILE__, __func__, __LINE__);
        plog_->fileOut_.close();
        delete plog_;
        plog_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------
LogCpp::LogCpp()
    : minLevel_(PRIO_DEBUG)
    , dir_("../log")
    , prefix_("log")
    , curDate_()
    , curIndex_(0)
    , maxSize_(5 * 1024 * 1024)   // 5MB
{
    // 注意：构造函数里不要调用 init()/write()。
    // 使用方（main.cpp）会立即显式调用 init() 指定真实日志目录；
    // 若在此处按默认 "../log" 初始化，会在进程 CWD 的上级目录
    // 创建多余的 log 文件夹。
    // 未显式 init() 时的兜底行为：首次 write() 时按默认路径打开文件。
}

LogCpp::~LogCpp()
{
    if (fileOut_.is_open())
        fileOut_.close();
}

// ---------------------------------------------------------------------------
// 初始化：目录 + 前缀
// ---------------------------------------------------------------------------
void LogCpp::init(const std::string& dir, const std::string& prefix)
{
    // 注意：锁内绝不能再调用 write()（非递归 std::mutex，
    // 重入加锁会抛 std::system_error 导致程序终止）
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // 关闭旧文件
        if (fileOut_.is_open())
            fileOut_.close();

        dir_ = dir.empty() ? std::string("../log") : dir;
        prefix_ = prefix.empty() ? std::string("log") : prefix;

        // 创建目录（仅一级；多级请自行保证父目录存在）
#ifdef _WIN32
        _mkdir(dir_.c_str());
#else
        mkdir(dir_.c_str(), 0755);
#endif

        curDate_.clear();
        curIndex_ = 0;

        // 打开当天文件
        ensureFileForToday();
    }

    write(PRIO_INFO, "LogCpp create success", __FILE__, "LogCpp", __LINE__);
}

// ---------------------------------------------------------------------------
// 级别设置
// ---------------------------------------------------------------------------
void LogCpp::setPriority(Priority priority)
{
    std::lock_guard<std::mutex> lock(mutex_);
    minLevel_ = priority;
}

const char* LogCpp::levelName(Priority level)
{
    switch (level)
    {
    case PRIO_ERROR: return "ERROR";
    case PRIO_WARN:  return "WARN";
    case PRIO_INFO:  return "INFO";
    case PRIO_DEBUG: return "DEBUG";
    default:    return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// 日期字符串 YYYYMMDD
// ---------------------------------------------------------------------------
std::string LogCpp::currentDateStr()
{
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tmBuf);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// 构造文件名：<prefix>_log.txt / <prefix>_log1.txt / ...
// ---------------------------------------------------------------------------
std::string LogCpp::buildFileName(int index) const
{
    std::ostringstream oss;
    oss << dir_ << "/" << prefix_ << "_log";
    if (index > 0)
        oss << index;
    oss << ".txt";
    return oss.str();
}

// ---------------------------------------------------------------------------
// 确保当前日期对应的文件已打开；若日期变了则重置序号
// 注意：调用者需已持有 mutex_
// ---------------------------------------------------------------------------
void LogCpp::ensureFileForToday()
{
    std::string today = currentDateStr();

    if (today != curDate_)
    {
        // 日期变化：关闭旧文件，序号归零
        if (fileOut_.is_open())
            fileOut_.close();
        curDate_ = today;
        curIndex_ = 0;
    }

    if (!fileOut_.is_open())
    {
        // 从 index=0 开始找一个可写文件；
        // 若当天已有文件且超过 5MB，则自动滚动
        int idx = curIndex_;
        while (true)
        {
            std::string path = buildFileName(idx);
            struct stat st;
            bool exists = (stat(path.c_str(), &st) == 0);

            if (exists && static_cast<uint64_t>(st.st_size) >= maxSize_)
            {
                ++idx;
                continue;
            }

            fileOut_.open(path.c_str(), std::ios::out | std::ios::app);
            if (fileOut_.is_open())
            {
                curIndex_ = idx;
                break;
            }
            // 打开失败，尝试下一个序号，避免死循环
            ++idx;
            if (idx > 10000)
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// 滚动到下一个文件
// 注意：调用者需已持有 mutex_
// ---------------------------------------------------------------------------
void LogCpp::openNextFile()
{
    if (fileOut_.is_open())
        fileOut_.close();

    ++curIndex_;

    // 跳过已存在且超限的文件
    while (true)
    {
        std::string path = buildFileName(curIndex_);
        struct stat st;
        bool exists = (stat(path.c_str(), &st) == 0);
        if (exists && static_cast<uint64_t>(st.st_size) >= maxSize_)
        {
            ++curIndex_;
            continue;
        }

        fileOut_.open(path.c_str(), std::ios::out | std::ios::app);
        if (fileOut_.is_open())
            break;

        ++curIndex_;
        if (curIndex_ > 10000)
            break;
    }
}

// ---------------------------------------------------------------------------
// 带调用位置的日志入口（宏调用）
// ---------------------------------------------------------------------------
void LogCpp::logWithSite(Priority level, const char* msg,
                        const char* file, const char* func, int line)
{
    write(level, msg, file, func, line);
}

// ---------------------------------------------------------------------------
// 实际写日志
// ---------------------------------------------------------------------------
void LogCpp::write(Priority level, const char* msg,
                  const char* file, const char* func, int line)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (level < minLevel_)
            return;
    }

    // 时间戳
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);

    std::tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S")
        << "," << std::setfill('0') << std::setw(3) << ms.count()
        << ": " << std::setfill(' ') << std::left << std::setw(5)
        << levelName(level) << " :";

    if (file)
    {
        std::string fname = file;
        size_t pos = fname.find_last_of("/\\");
        if (pos != std::string::npos)
            fname = fname.substr(pos + 1);

        oss << "[" << fname << ":" << (func ? func : "") << ":"
            << line << "] ";
    }

    oss << (msg ? msg : "") << "\n";
    std::string lineStr = oss.str();

    std::lock_guard<std::mutex> lock(mutex_);

    // 日期切换 + 文件打开
    ensureFileForToday();

    // 写前检查大小：超过 5MB 就滚动
    if (fileOut_.is_open())
    {
        fileOut_.seekp(0, std::ios::end);
        std::streamoff sz = fileOut_.tellp();
        if (sz >= 0 && static_cast<uint64_t>(sz) >= maxSize_)
            openNextFile();
    }

    std::cout << lineStr;
    std::cout.flush();

    if (fileOut_.is_open())
    {
        fileOut_ << lineStr;
        fileOut_.flush();
    }
}

// ---------------------------------------------------------------------------
// 便捷方法（不带调用位置）
// ---------------------------------------------------------------------------
void LogCpp::error(const char* msg) { write(PRIO_ERROR, msg, nullptr, nullptr, 0); }
void LogCpp::warn(const char* msg)  { write(PRIO_WARN,  msg, nullptr, nullptr, 0); }
void LogCpp::info(const char* msg)  { write(PRIO_INFO,  msg, nullptr, nullptr, 0); }
void LogCpp::debug(const char* msg) { write(PRIO_DEBUG, msg, nullptr, nullptr, 0); }