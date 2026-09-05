#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "Framework/HttpMessage.h"
#include "Module/Tenant/CTenant.h"

namespace datahub {

using sc::CTenant;

/// @brief 在线成员服务（按租户隔离）。
///
/// 以"租户 + 客户端标识"为键跟踪成员（客户端标识取 X-Client-Id，浏览器持久化
/// UUID）：成员属于其活跃的租户，/api/members 只返回当前租户的活跃者。
/// 30 秒无活跃自动移除。基于框架 CHttpRequest，不依赖 workflow 类型。
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
    static std::string ClientId(web::CHttpRequest& req);

    // 记录成员在指定租户的活跃（仅当请求携带 X-Client-Id 头）；返回客户端标识。
    std::string Touch(web::CHttpRequest& req, const CTenant& tenant);

    // 清理超过 30 秒未活跃的成员（全部租户；返回清理数量）。
    size_t Prune();

    // 指定租户的成员快照（客户端标识 → 成员信息）。
    std::map<std::string, MemberInfo> Snapshot(const CTenant& tenant) const;

    // 指定租户的当前成员数。
    size_t Count(const CTenant& tenant) const;

    // 清空全部租户的成员。
    void Clear();

   private:
    mutable std::mutex m_mutex;
    std::map<std::string, std::map<std::string, MemberInfo> > m_mapByTenant;  // [租户码][客户端标识]
};

}  // namespace datahub
