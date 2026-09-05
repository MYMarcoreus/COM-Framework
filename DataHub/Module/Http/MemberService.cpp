#include "Module/Http/MemberService.h"

#include <chrono>
#include <string>

namespace datahub {

namespace {
/// @brief 当前时间（毫秒）。
std::int64_t NowMs()
{
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}
}  // namespace

CMemberService::CMemberService() {}

/// @brief 获取客户端标识。
///
/// 优先取 X-Client-Id 请求头（前端持久化 UUID，跨请求标识同一浏览器）；
/// 无该头时退回 "IP:port"（如 curl 等命令行访问）。
std::string CMemberService::ClientId(web::CHttpRequest& req)
{
    std::string strId = req.Header("X-Client-Id");
    if (strId.empty())
    {
        strId = req.Peer();
    }
    return strId;
}

/// @brief 记录成员活跃（每次请求调用）；返回客户端标识。
std::string CMemberService::Touch(web::CHttpRequest& req)
{
    // 仅当请求携带有效 X-Client-Id 头时记录成员；无头请求（curl、静态资源、
    // 探测等）不计入，避免产生 "IP:port" 假成员。
    std::string strHeader = req.Header("X-Client-Id");
    if (strHeader.empty())
    {
        return std::string();
    }

    std::string strClientId = ClientId(req);
    if (strClientId.empty() || strClientId == "unknown:0")
    {
        return strClientId;
    }
    // 提取来源 IP（不含端口），供成员展示。
    std::string strIp = req.Peer();
    std::string::size_type nColon = strIp.find_last_of(':');
    if (nColon != std::string::npos)
    {
        strIp = strIp.substr(0, nColon);
    }
    std::int64_t nNowMs = NowMs();
    std::lock_guard<std::mutex> lock(m_mutex);
    MemberInfo& info = m_mapMembers[strClientId];
    info.strIp = strIp;
    if (info.nFirstMs == 0)
    {
        info.nFirstMs = nNowMs;
    }
    info.nLastMs = nNowMs;
    return strClientId;
}

/// @brief 清理超过 30 秒未活跃的成员（返回清理数量）。
size_t CMemberService::Prune()
{
    const std::int64_t nTimeoutMs = 30000;  // 30 秒
    std::int64_t nNowMs = NowMs();
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t nRemoved = 0;
    for (auto it = m_mapMembers.begin(); it != m_mapMembers.end();)
    {
        if (nNowMs - it->second.nLastMs > nTimeoutMs)
        {
            it = m_mapMembers.erase(it);
            ++nRemoved;
        }
        else
        {
            ++it;
        }
    }
    return nRemoved;
}

std::map<std::string, CMemberService::MemberInfo> CMemberService::Snapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_mapMembers;
}

size_t CMemberService::Count() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_mapMembers.size();
}

void CMemberService::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_mapMembers.clear();
}

}  // namespace datahub
