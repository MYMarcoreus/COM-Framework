#include "Module/MemberTracker.h"

#include <chrono>

#include "Module/HttpUtil.h"

namespace datahub {

std::map<std::string, MemberTracker::MemberInfo> MemberTracker::s_mapMembers;
std::mutex MemberTracker::s_mutex;

/// @brief 当前时间（毫秒）。
static std::int64_t NowMs()
{
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

/// @brief 获取客户端标识。
///
/// 优先取 X-Client-Id 请求头（前端生成并持久化的 UUID，用于跨请求标识同一浏览器）；
/// 无该头时退回 "IP:port"（如 curl 等命令行访问）。
std::string MemberTracker::ClientId(WFHttpTask* pServerTask)
{
    std::string strId = HttpUtil::GetHeader(pServerTask, "X-Client-Id");
    if (strId.empty())
    {
        strId = HttpUtil::PeerAddress(pServerTask);
    }
    return strId;
}

/// @brief 记录成员活跃（每次请求调用）；返回客户端标识。
std::string MemberTracker::Touch(WFHttpTask* pServerTask)
{
    std::string strClientId = ClientId(pServerTask);
    if (strClientId.empty() || strClientId == "unknown:0")
    {
        return strClientId;
    }
    // 提取来源 IP（不含端口），供成员展示。
    std::string strIp = HttpUtil::PeerAddress(pServerTask);
    std::string::size_type nColon = strIp.find_last_of(':');
    if (nColon != std::string::npos)
    {
        strIp = strIp.substr(0, nColon);
    }
    std::int64_t nNowMs = NowMs();
    std::lock_guard<std::mutex> lock(s_mutex);
    MemberInfo& info = s_mapMembers[strClientId];
    info.strIp = strIp;
    if (info.nFirstMs == 0)
    {
        info.nFirstMs = nNowMs;
    }
    info.nLastMs = nNowMs;
    return strClientId;
}

/// @brief 清理超过 30 秒未活跃的成员（返回清理数量）。
size_t MemberTracker::Prune()
{
    const std::int64_t nTimeoutMs = 30000; // 30 秒
    std::int64_t nNowMs = NowMs();
    std::lock_guard<std::mutex> lock(s_mutex);
    size_t nRemoved = 0;
    for (auto it = s_mapMembers.begin(); it != s_mapMembers.end();)
    {
        if (nNowMs - it->second.nLastMs > nTimeoutMs)
        {
            it = s_mapMembers.erase(it);
            ++nRemoved;
        }
        else
        {
            ++it;
        }
    }
    return nRemoved;
}

/// @brief 成员快照。
std::map<std::string, MemberTracker::MemberInfo> MemberTracker::Snapshot()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_mapMembers;
}

size_t MemberTracker::Count()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_mapMembers.size();
}

void MemberTracker::Clear()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_mapMembers.clear();
}

} // namespace datahub
