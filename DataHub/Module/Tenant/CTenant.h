#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sc {

/// @brief 租户容量限制（0 = 不限制）。作为 CTenant 的属性随实体传递。
struct CTenantLimits
{
    std::size_t nMaxItems = 0;         // 最大数据条数
    std::uint64_t nMaxTotalBytes = 0;  // 内容总字节上限
    std::uint64_t nMaxItemBytes = 0;   // 单条内容字节上限
};

/// @brief 租户领域实体。
///
/// DataHub 的数据隔离单元。每个租户拥有独立码 / 名称 /
/// 容量限制与创建时间；消息、文件、成员、配额均以租户为边界隔离。
/// 内置公共租户码 "public"（缺省 X-Tenant 时使用，保持向后兼容）。
struct CTenant
{
    std::string strCode;         // 租户码（分享码；"public" 为公共租户）
    std::string strName;         // 展示名称
    CTenantLimits limits;        // 该租户容量限制
    std::int64_t nCreateMs = 0;  // 创建时间（毫秒）
};

/// @brief 租户内角色。
///
/// 账号 = 客户端标识（X-Client-Id，浏览器持久化 UUID）；角色决定授权：
/// Owner 可删除任意数据 / 管理成员，Member 可读写 / 管理自己创建的数据。
enum class TenantRole : int
{
    kOwner = 0,  // 所有者
    kMember = 1  // 成员
};

/// @brief 租户成员（账号 + 角色）。
struct CTenantMember
{
    std::string strAccountId;  // 账号（客户端标识 X-Client-Id）
    TenantRole role;           // 角色
    std::int64_t nJoinMs = 0;  // 加入时间（毫秒）
};

}  // namespace sc
