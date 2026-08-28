#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

namespace common {
namespace log {

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
/// 提供等级过滤与时间戳格式化；可配置单文件按大小滚动（保留备份）。
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
    bool OpenFile(const std::string& strPath);

    // 设置单个日志文件最大字节数；超过后滚动（保留 kMaxBackupCount 个备份）。
    // nMaxBytes 为 0 表示不滚动。
    void SetMaxFileSize(std::uint64_t nMaxBytes);

    // 输出一条日志。
    void Log(LogLevel level, const std::string& strMessage);

    // 便捷方法。
    void Trace(const std::string& strMessage);
    void Debug(const std::string& strMessage);
    void Info(const std::string& strMessage);
    void Warn(const std::string& strMessage);
    void Error(const std::string& strMessage);

private:
    CLogger();
    ~CLogger();

    CLogger(const CLogger&) = delete;
    CLogger& operator=(const CLogger&) = delete;

    // 生成时间戳字符串。
    static std::string FormatTime();

    // 返回级别名称。
    static const char* LevelName(LogLevel level);

    // 按大小滚动日志文件（持有 m_mutex 时调用）。
    void RotateIfNeeded();

    // 返回文件字节数；失败返回 0。
    static std::uint64_t FileSize(const std::string& strPath);

    mutable std::mutex m_mutex;
    LogLevel m_level;
    std::ofstream m_file;
    bool m_bFileEnabled;
    std::string m_strFilePath;
    std::uint64_t m_nMaxFileBytes; // 0 = 不滚动

    // 保留的滚动备份文件个数（xxx.log.1 .. xxx.log.kMaxBackupCount）。
    static const int kMaxBackupCount = 5;
};

} // namespace log
} // namespace common
