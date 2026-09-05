#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "Module/InterfaceMap.h"
#include "Module/Module.h"
#include "Module/Tenant/ITenantService.h"

namespace datahub {

using sc::CTenant;
using sc::CTenantLimits;
using sc::ITenantService;

/// @brief 租户注册表模块（运行时模块，暴露 ITenantService）。
///
/// 生命周期持有全部租户实体；构造时即内置公共租户（码 "public"），
/// 保证任何缺省请求都能解析到默认租户。新租户码由本模块生成（短码），
/// 容量限制统一采用构造注入的默认值（来自 [store] 配置）。
/// 模块名 "tenant"。
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
    bool CreateTenant(const std::string& strName, CTenant& out) override;
    bool FindTenant(const std::string& strCode, CTenant& out) const override;
    std::size_t Count() const override;

    SC_DECLARE_INTERFACE_MAP();

   private:
    // 生成不与现有租户冲突的短码。
    std::string GenerateCode() const;

    std::string m_strDefaultCode;  // "public"
    CTenantLimits m_defaultLimits;
    mutable std::mutex m_mutex;
    std::map<std::string, CTenant> m_mapTenants;
};

}  // namespace datahub
