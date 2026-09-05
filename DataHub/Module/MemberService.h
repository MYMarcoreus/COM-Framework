#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "workflow/WFHttpServer.h"

namespace datahub {

/// @brief 在线成员服务（实例化，替代原静态 MemberTracker）。
///
/// 以客户端标识（X-Client-Id 请求头，浏览器持久化 UUID）为 key 跟踪成员，
/// 同一浏览器的多次轮询只计为 1 个成员。30 秒无活跃自动移除。
/// 实例可注入、可多开、可单测。
class CMemberService
{
   public:
    CMemberService();

    // 在线成员信息。
    struct MemberInfo
    {
        std::string strIp;      // 来源 IP
        std::int64_t nFirstMs;  // 首次活跃时间（毫秒）
        std::int64_t nLastMs;   // 最后活跃时间（毫秒）
    };

    // 获取客户端标识：优先取 X-Client-Id 请求头，否则退回 "IP:port"。
    static std::string ClientId(WFHttpTask* pServerTask);

    // 记录成员活跃（仅当请求携带 X-Client-Id 头）；返回客户端标识（无头返回空串）。
    std::string Touch(WFHttpTask* pServerTask);

    // 清理超过 30 秒未活跃的成员（返回清理数量）。
    size_t Prune();

    // 成员快照（客户端标识 → 成员信息）。
    std::map<std::string, MemberInfo> Snapshot() const;

    // 当前成员数。
    size_t Count() const;

    // 清空全部成员。
    void Clear();

   private:
    mutable std::mutex m_mutex;
    std::map<std::string, MemberInfo> m_mapMembers;
};

}  // namespace datahub
