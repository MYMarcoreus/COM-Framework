#pragma once

#include <cstddef>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

#include "Protocol/LogProtocol.h"

namespace logserver {

/// @brief 日志存储。
///
/// 集中收集日志并落盘：按来源分文件，写入 <目录>/<source>.log。
/// 线程安全，供网络回调线程并发调用。
/// 每条日志格式：<本地时间> [<级别>] <内容>\n。
class CLogStorage
{
public:
    CLogStorage();

    ~CLogStorage();

    // 设置存储目录（首次写入前调用）；目录不存在时自动创建。
    bool SetDirectory(const std::string& strDir);

    // 存储目录。
    const std::string& Directory() const;

    // 写入一条日志记录。
    bool Write(const LogRecord& record);

    // 当前已打开的文件数（供状态报告）。
    size_t FileCount() const;

private:
    // 将来源清理为安全的文件名（仅保留字母/数字/_/-/.，防路径穿越）。
    static std::string SanitizeSource(const std::string& strSource);

    // 将 epoch 秒格式化为本地时间字符串 YYYY-MM-DD HH:MM:SS。
    static std::string FormatTime(std::uint64_t nEpochSeconds);

    // 级别字符串转大写。
    static std::string Upper(const std::string& strLevel);

    // 递归创建目录。
    static bool CreateDirectories(const std::string& strPath);

    // 按来源获取文件输出流（不存在则创建并打开）。
    std::ofstream& StreamFor(const std::string& strSource);

    std::string m_strDirectory;
    std::map<std::string, std::ofstream> m_mapFiles;
    mutable std::mutex m_mutex;
};

} // namespace logserver
