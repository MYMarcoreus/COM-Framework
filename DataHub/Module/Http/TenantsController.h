#pragma once

#include <string>

#include "Framework/HttpMessage.h"
#include "Module/Tenant/ITenantService.h"

namespace datahub {

namespace web { class CHttpRouter; }

/// @brief 租户（资源）控制器 —— 跨租户的平台能力，与"租户内业务"分离。
///
/// 负责租户的生命周期管理：创建租户、按码查询租户（供凭码加入前校验）。
/// 与 CHttpHandlers（租户内消息/文件/成员业务）解耦；路由注册到同一
/// CHttpRouter，由装配层编排。依赖 ITenantService（租户注册表）。
class CTenantsController
{
   public:
    // @param pTenants 租户注册表（ITenantService，装配层注入）。
    explicit CTenantsController(sc::ITenantService* pTenants);

    // 注册本控制器负责的路由（POST /api/tenant、GET /api/tenant/info）。
    void RegisterRoutes(web::CHttpRouter& router);

    // 创建租户：POST /api/tenant（名称为请求体，UTF-8 非空）。返回 {code,name}。
    bool HandleCreate(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 查询租户：GET /api/tenant/info?code=xxx（供凭码加入前校验存在性）。
    bool HandleInfo(web::CHttpRequest& req, web::CHttpResponse& resp);

   private:
    sc::ITenantService* m_pTenants;  // 租户注册表（生命周期由装配层管理）
};

}  // namespace datahub
