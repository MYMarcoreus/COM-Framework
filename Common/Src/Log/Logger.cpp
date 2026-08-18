#include "Log/Logger.h"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace common {

/// @brief 获取日志器单例。
Logger& Logger::Instance()
{
    static Logger instance;
    return instance;
}

/// @brief 创建日志器。
Logger::Logger() : level_(LogLevel::kInfo), fileEnabled_(false)
{
}

/// @brief 销毁日志器。
Logger::~Logger()
{
    if (file_.is_open())
    {
        file_.close();
    }
}

/// @brief 设置日志级别。
void Logger::SetLevel(LogLevel level)
{
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

/// @brief 返回当前日志级别。
LogLevel Logger::Level() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return level_;
}

/// @brief 启用文件输出。
bool Logger::OpenFile(const std::string& path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    file_.close();
    file_.open(path.c_str(), std::ios::out | std::ios::app);
    if (!file_.is_open())
    {
        fileEnabled_ = false;
        return false;
    }
    fileEnabled_ = true;
    return true;
}

/// @brief 输出一条日志。
///
/// ① 过滤低于当前级别的日志。
/// ② 格式化后输出到控制台与文件（可选）。
void Logger::Log(LogLevel level, const std::string& message)
{
    // ① 级别过滤
    LogLevel current;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current = level_;
    }
    if (level < current || level == LogLevel::kOff)
    {
        return;
    }
    // ② 格式化并输出
    std::string line = "[" + FormatTime() + "] [" + LevelName(level) + "] " + message + "\n";
    std::lock_guard<std::mutex> lock(mutex_);
    std::fwrite(line.c_str(), 1, line.size(), stdout);
    std::fflush(stdout);
    if (fileEnabled_ && file_.is_open())
    {
        file_ << line;
        file_.flush();
    }
}

/// @brief 输出跟踪日志。
void Logger::Trace(const std::string& message)
{
    Log(LogLevel::kTrace, message);
}

/// @brief 输出调试日志。
void Logger::Debug(const std::string& message)
{
    Log(LogLevel::kDebug, message);
}

/// @brief 输出信息日志。
void Logger::Info(const std::string& message)
{
    Log(LogLevel::kInfo, message);
}

/// @brief 输出警告日志。
void Logger::Warn(const std::string& message)
{
    Log(LogLevel::kWarn, message);
}

/// @brief 输出错误日志。
void Logger::Error(const std::string& message)
{
    Log(LogLevel::kError, message);
}

/// @brief 生成时间戳字符串。
std::string Logger::FormatTime()
{
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf;
    localtime_r(&t, &tmBuf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmBuf);
    return std::string(buf);
}

/// @brief 返回级别名称。
const char* Logger::LevelName(LogLevel level)
{
    switch (level)
    {
    case LogLevel::kTrace: return "TRACE";
    case LogLevel::kDebug: return "DEBUG";
    case LogLevel::kInfo:  return "INFO";
    case LogLevel::kWarn:  return "WARN";
    case LogLevel::kError: return "ERROR";
    default:               return "?";
    }
}

} // namespace common
