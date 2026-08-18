#pragma once

#include <fstream>
#include <mutex>
#include <string>

namespace common {

/// @brief 日志级别。
enum class LogLevel
{
    kTrace = 0, // 跟踪
    kDebug = 1, // 调试
    kInfo = 2,  // 信息
    kWarn = 3,  // 警告
    kError = 4, // 错误
    kOff = 5,   // 关闭
};

/// @brief 日志器。
///
/// 线程安全，输出到控制台并可同时输出到文件。
/// 提供等级过滤与时间戳格式化。
class CLogger
{
public:
    // 获取全局单例。
    static CLogger& Instance();

    // 设置日志级别（低于该级别的日志被丢弃）。
    void SetLevel(LogLevel level);

    // 返回当前日志级别。
    LogLevel Level() const;

    // 启用文件输出。
    bool OpenFile(const std::string& path);

    // 输出一条日志。
    void Log(LogLevel level, const std::string& message);

    // 便捷方法。
    void Trace(const std::string& message);
    void Debug(const std::string& message);
    void Info(const std::string& message);
    void Warn(const std::string& message);
    void Error(const std::string& message);

private:
    CLogger();
    ~CLogger();

    CLogger(const CLogger&) = delete;
    CLogger& operator=(const CLogger&) = delete;

    // 生成时间戳字符串。
    static std::string FormatTime();

    // 返回级别名称。
    static const char* LevelName(LogLevel level);

    mutable std::mutex m_mutex;
    LogLevel m_level;
    std::ofstream m_file;
    bool m_bFileEnabled;
};

} // namespace common
