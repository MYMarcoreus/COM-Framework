#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "Module/IUnknown.h"
#include "Module/InterfaceDecl.h"
#include "Module/Tenant/CTenant.h"

namespace sc {

/// @brief 租户注册表 + 成员接口（COM 风格）。
///
/// 持有全部租户及其容量限制与成员花名册（账号+角色），是"请求 → 租户"
/// 解析与授权判定的唯一来源。账号 = 客户端标识（X-Client-Id）。
/// 内置默认租户（公共租户，码 "public"），缺省请求回落其上。
SC_INTERFACE(ITenantService, "datahub::ITenantService", "4d9fa6e2-1c73-4b8e-9f41-a5c6d0b7e3f2")
{
   public:
    virtual ~ITenantService() {}

    // 默认租户码（公共租户："public"；未携带 X-Tenant 时使用）。
    virtual const std::string& DefaultCode() const = 0;

    // 创建租户（名称可为空；ownerAccount 为创建者账号，非空则成为 Owner），
    // 生成唯一码到 out。@return true 创建成功。
    virtual bool CreateTenant(const std::string& strName, const std::string& strOwnerAccount, CTenant& out) = 0;

    // 按码查找租户（含默认租户）；不存在返回 false。
    virtual bool FindTenant(const std::string& strCode, CTenant& out) const = 0;

    // 账号凭码加入租户：未在花名册则作为 Member 加入；返回其角色到 out。
    // 公共租户恒为 Member。@return false = 租户不存在 / 账号为空。
    virtual bool JoinTenant(const std::string& strCode, const std::string& strAccount, TenantRole& out) = 0;

    // 查询账号在某租户的角色；公共租户恒为 Member；未加入返回 false。
    virtual bool TenantRoleOf(const std::string& strCode, const std::string& strAccount, TenantRole& out) const = 0;

    // 某租户的成员花名册（公共租户为空）。@return true。
    virtual bool ListMembers(const std::string& strCode, std::vector<CTenantMember>& vecOut) const = 0;

    // 当前租户数量。
    virtual std::size_t Count() const = 0;
};

}  // namespace sc
