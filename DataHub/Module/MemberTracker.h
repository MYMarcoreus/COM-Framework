#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "workflow/WFHttpServer.h"

namespace datahub {

/// @brief 在线成员跟踪。
///
/// 以客户端标识（X-Client-Id 请求头，浏览器持久化 UUID）为 key 跟踪成员，
/// 同一浏览器的多次轮询只计为 1 个成员（避免按 IP:port 记录时因
/// 每次请求端口变化导致成员膨胀）。30 秒无活跃自动移除。
class MemberTracker
{
public:
    // 在线成员信息。
    struct MemberInfo
    {
        std::string strIp;      // 来源 IP
        std::int64_t nFirstMs;  // 首次活跃时间（毫秒）
        std::int64_t nLastMs;   // 最后活跃时间（毫秒）
    };

    // 获取客户端标识：优先取 X-Client-Id 请求头，否则退回 "IP:port"。
    static std::string ClientId(WFHttpTask* pServerTask);

    // 记录成员活跃（每次请求调用）；返回客户端标识。
    static std::string Touch(WFHttpTask* pServerTask);

    // 清理超过 30 秒未活跃的成员（返回清理数量）。
    static size_t Prune();

    // 成员快照（客户端标识 → 成员信息）；内部加锁。
    static std::map<std::string, MemberInfo> Snapshot();

    // 当前成员数。
    static size_t Count();

    // 清空全部成员。
    static void Clear();

private:
    static std::map<std::string, MemberInfo> s_mapMembers;
    static std::mutex s_mutex;
};

} // namespace datahub
