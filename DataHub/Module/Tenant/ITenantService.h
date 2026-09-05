#pragma once

#include <cstddef>
#include <string>

#include "Module/IUnknown.h"
#include "Module/InterfaceDecl.h"
#include "Module/Tenant/CTenant.h"

namespace sc {

/// @brief 租户注册表接口（COM 风格）。
///
/// 持有全部租户（空间）及其容量限制，是"请求 → 租户"解析的唯一来源：
/// Http 模块在入口按 X-Space 解析租户后挂到请求上下文，业务不再各自解析。
/// 内置默认租户（公共空间，码 "public"），缺省请求回落其上。
SC_INTERFACE(ITenantService, "datahub::ITenantService", "4d9fa6e2-1c73-4b8e-9f41-a5c6d0b7e3f2")
{
   public:
    virtual ~ITenantService() {}

    // 默认租户码（公共空间："public"；未携带 X-Space 时使用）。
    virtual const std::string& DefaultCode() const = 0;

    // 创建租户（名称可为空；使用模块统一默认容量限制），生成唯一码到 out。
    // @return true 创建成功。
    virtual bool CreateTenant(const std::string& strName, CTenant& out) = 0;

    // 按码查找租户（含默认租户）；不存在返回 false。
    virtual bool FindTenant(const std::string& strCode, CTenant& out) const = 0;

    // 当前租户数量。
    virtual std::size_t Count() const = 0;
};

}  // namespace sc
