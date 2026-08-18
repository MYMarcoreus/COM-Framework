#include "Log/Logger.h"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace common {

/// @brief 获取日志器单例。
CLogger& CLogger::Instance()
{
    static CLogger instance;
    return instance;
}

/// @brief 创建日志器。
CLogger::CLogger() : m_level(LogLevel::kInfo), m_bFileEnabled(false)
{
}

/// @brief 销毁日志器。
CLogger::~CLogger()
{
    if (m_file.is_open())
    {
        m_file.close();
    }
}

/// @brief 设置日志级别。
void CLogger::SetLevel(LogLevel level)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_level = level;
}

/// @brief 返回当前日志级别。
LogLevel CLogger::Level() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_level;
}

/// @brief 启用文件输出。
bool CLogger::OpenFile(const std::string& strPath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_file.close();
    m_file.open(strPath.c_str(), std::ios::out | std::ios::app);
    if (!m_file.is_open())
    {
        m_bFileEnabled = false;
        return false;
    }
    m_bFileEnabled = true;
    return true;
}

/// @brief 输出一条日志。
///
/// ① 过滤低于当前级别的日志。
/// ② 格式化后输出到控制台与文件（可选）。
void CLogger::Log(LogLevel level, const std::string& strMessage)
{
    // ① 级别过滤
    LogLevel levelCurrent;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        levelCurrent = m_level;
    }
    if (level < levelCurrent || level == LogLevel::kOff)
    {
        return;
    }
    // ② 格式化并输出
    std::string strLine = "[" + FormatTime() + "] [" + LevelName(level) + "] " + strMessage + "\n";
    std::lock_guard<std::mutex> lock(m_mutex);
    std::fwrite(strLine.c_str(), 1, strLine.size(), stdout);
    std::fflush(stdout);
    if (m_bFileEnabled && m_file.is_open())
    {
        m_file << strLine;
        m_file.flush();
    }
}

/// @brief 输出跟踪日志。
void CLogger::Trace(const std::string& strMessage)
{
    Log(LogLevel::kTrace, strMessage);
}

/// @brief 输出调试日志。
void CLogger::Debug(const std::string& strMessage)
{
    Log(LogLevel::kDebug, strMessage);
}

/// @brief 输出信息日志。
void CLogger::Info(const std::string& strMessage)
{
    Log(LogLevel::kInfo, strMessage);
}

/// @brief 输出警告日志。
void CLogger::Warn(const std::string& strMessage)
{
    Log(LogLevel::kWarn, strMessage);
}

/// @brief 输出错误日志。
void CLogger::Error(const std::string& strMessage)
{
    Log(LogLevel::kError, strMessage);
}

/// @brief 生成时间戳字符串。
std::string CLogger::FormatTime()
{
    std::chrono::system_clock::time_point tmNow = std::chrono::system_clock::now();
    std::time_t nTime = std::chrono::system_clock::to_time_t(tmNow);
    std::tm tmBuf;
    localtime_r(&nTime, &tmBuf);
    char szBuf[32];
    std::strftime(szBuf, sizeof(szBuf), "%Y-%m-%d %H:%M:%S", &tmBuf);
    return std::string(szBuf);
}

/// @brief 返回级别名称。
const char* CLogger::LevelName(LogLevel level)
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
