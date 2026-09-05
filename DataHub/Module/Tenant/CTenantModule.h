#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "Module/InterfaceMap.h"
#include "Module/Module.h"
#include "Module/Tenant/ITenantService.h"

namespace datahub {

using sc::CTenant;
using sc::CTenantLimits;
using sc::ITenantService;

/// @brief 租户注册表 + 成员模块（运行时模块，暴露 ITenantService）。
///
/// 生命周期持有全部租户实体与成员花名册；构造时即内置公共租户（码
/// "public"，无 Owner，人人皆 Member），保证任何缺省请求都能解析到默认
/// 租户。新租户码由本模块生成（短码）；容量限制统一采用构造注入的默认值。
/// 创建者（账号）成为 Owner，凭码加入者成为 Member。模块名 "tenant"。
class CTenantModule : public sc::CModule, public ITenantService
{
   public:
    // @param defaultLimits 新建租户统一采用的默认容量限制。
    explicit CTenantModule(const CTenantLimits& defaultLimits);

    virtual ~CTenantModule();

    bool Initialize(const sc::CResolveContext& ctx) override;
    bool Start() override;
    void Stop() override;
    void Shutdown() override;

    const std::string& DefaultCode() const override;
    bool CreateTenant(const std::string& strName, const std::string& strOwnerAccount, CTenant& out) override;
    bool FindTenant(const std::string& strCode, CTenant& out) const override;
    bool JoinTenant(const std::string& strCode, const std::string& strAccount, sc::TenantRole& out) override;
    bool TenantRoleOf(const std::string& strCode, const std::string& strAccount, sc::TenantRole& out) const override;
    bool ListMembers(const std::string& strCode, std::vector<sc::CTenantMember>& vecOut) const override;
    void ListTenants(std::vector<CTenant>& vecOut) const override;
    bool RenameTenant(const std::string& strCode, const std::string& strName) override;
    bool SetTenantLimits(const std::string& strCode, const CTenantLimits& limits) override;
    bool RemoveTenant(const std::string& strCode) override;
    bool RemoveMember(const std::string& strCode, const std::string& strAccount) override;
    bool SetMemberRole(const std::string& strCode, const std::string& strAccount, sc::TenantRole role) override;
    std::size_t Count() const override;

    SC_DECLARE_INTERFACE_MAP();

   private:
    // 生成不与现有租户冲突的短码。
    std::string GenerateCode() const;

    // 某租户内 Owner 数量（须持锁调用）。
    std::size_t OwnerCountLocked(const std::string& strCode) const;

    std::string m_strDefaultCode;  // "public"
    CTenantLimits m_defaultLimits;
    mutable std::mutex m_mutex;
    std::map<std::string, CTenant> m_mapTenants;                                    // 租户实体
    std::map<std::string, std::map<std::string, sc::CTenantMember> > m_mapMembers;  // [租户码][账号]
};

}  // namespace datahub
