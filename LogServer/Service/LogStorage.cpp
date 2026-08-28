#include "Service/LogStorage.h"

#include <cerrno>
#include <cctype>
#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>

#include "Log/Logger.h"

namespace logserver {

/// @brief 创建日志存储。
CLogStorage::CLogStorage()
{
}

/// @brief 销毁日志存储。
CLogStorage::~CLogStorage()
{
}

/// @brief 设置存储目录。
///
/// 目录不存在时自动递归创建；创建失败时目录保持为空（写入将失败）。
///
/// @param strDir 存储目录。
///
/// @return true 设置成功；false 参数为空或目录创建失败。
bool CLogStorage::SetDirectory(const std::string& strDir)
{
    if (strDir.empty())
    {
        return false;
    }
    if (!CreateDirectories(strDir))
    {
        common::log::CLogger::Instance().Error(
            "[CLogStorage] 创建日志目录失败: " + strDir);
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_strDirectory = strDir;
    return true;
}

/// @brief 存储目录。
const std::string& CLogStorage::Directory() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_strDirectory;
}

/// @brief 写入一条日志记录。
///
/// 按来源分文件追加写入，并立即落盘（flush），避免进程崩溃时丢失日志。
///
/// @param record 日志记录。
///
/// @return true 写入成功；false 未设置目录或打开文件失败。
bool CLogStorage::Write(const LogRecord& record)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_strDirectory.empty())
    {
        return false;
    }
    std::string strSource = SanitizeSource(record.strSource);
    if (strSource.empty())
    {
        strSource = "unknown";
    }
    std::ofstream& stream = StreamFor(strSource);
    if (!stream.is_open())
    {
        common::log::CLogger::Instance().Error(
            "[CLogStorage] 打开日志文件失败: " + strSource);
        return false;
    }
    stream << FormatTime(record.nTimestamp) << " [" << Upper(record.strLevel) << "] "
           << record.strContent << '\n';
    stream.flush();
    return true;
}

/// @brief 当前已打开的文件数。
size_t CLogStorage::FileCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_mapFiles.size();
}

/// @brief 将来源清理为安全的文件名。
///
/// 仅保留字母 / 数字 / _ / - / .，其余字符（含 '/'、'\' 等路径分隔符）全部丢弃，
/// 防止上报的来源被用于路径穿越写入任意文件。
///
/// @param strSource 原始来源。
///
/// @return 清理后的文件名（可能为空串）。
std::string CLogStorage::SanitizeSource(const std::string& strSource)
{
    std::string strOut;
    strOut.reserve(strSource.size());
    for (size_t i = 0; i < strSource.size(); ++i)
    {
        unsigned char c = static_cast<unsigned char>(strSource[i]);
        if (std::isalnum(c) || c == '_' || c == '-' || c == '.')
        {
            strOut.push_back(static_cast<char>(c));
        }
    }
    return strOut;
}

/// @brief 将 epoch 秒格式化为本地时间字符串。
///
/// @param nEpochSeconds epoch 秒。
///
/// @return 形如 "2026-08-19 10:00:00" 的字符串。
std::string CLogStorage::FormatTime(std::uint64_t nEpochSeconds)
{
    time_t tTime = static_cast<time_t>(nEpochSeconds);
    struct tm tmLocal;
    if (localtime_r(&tTime, &tmLocal) == nullptr)
    {
        return "0000-00-00 00:00:00";
    }
    char szBuf[32];
    std::strftime(szBuf, sizeof(szBuf), "%Y-%m-%d %H:%M:%S", &tmLocal);
    return std::string(szBuf);
}

/// @brief 级别字符串转大写。
std::string CLogStorage::Upper(const std::string& strLevel)
{
    std::string strOut = strLevel;
    for (size_t i = 0; i < strOut.size(); ++i)
    {
        strOut[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(strOut[i])));
    }
    return strOut;
}

/// @brief 递归创建目录。
///
/// @param strPath 目录路径（可为相对或绝对路径）。
///
/// @return true 创建成功或已存在；false 创建失败。
bool CLogStorage::CreateDirectories(const std::string& strPath)
{
    if (strPath.empty())
    {
        return false;
    }
    size_t nPos = 0;
    while (true)
    {
        size_t nNext = strPath.find('/', nPos);
        if (nNext == std::string::npos)
        {
            nNext = strPath.size();
        }
        std::string strCur = strPath.substr(0, nNext);
        if (!strCur.empty())
        {
            if (::mkdir(strCur.c_str(), 0755) != 0 && errno != EEXIST)
            {
                return false;
            }
        }
        if (nNext == strPath.size())
        {
            break;
        }
        nPos = nNext + 1;
    }
    return true;
}

/// @brief 按来源获取文件输出流。
///
/// 文件不存在时创建并以追加方式打开；路径为 <目录>/<source>.log。
/// 调用方须持有 m_mutex。
///
/// @param strSource 清理后的来源名。
///
/// @return 对应文件的输出流引用。
std::ofstream& CLogStorage::StreamFor(const std::string& strSource)
{
    std::map<std::string, std::ofstream>::iterator it = m_mapFiles.find(strSource);
    if (it == m_mapFiles.end())
    {
        std::string strPath = m_strDirectory + "/" + strSource + ".log";
        std::ofstream stream(strPath.c_str(), std::ios::app);
        it = m_mapFiles.emplace(strSource, std::move(stream)).first;
    }
    return it->second;
}

} // namespace logserver
