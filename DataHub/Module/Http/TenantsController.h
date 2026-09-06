#pragma once

#include <string>

#include "Framework/HttpMessage.h"
#include "Module/Http/RequestContext.h"
#include "Module/Tenant/ITenantService.h"

namespace datahub {

namespace web {
class CHttpRouter;
}

/// @brief 租户（资源）控制器 —— 跨租户的平台能力，与"租户内业务"分离。
///
/// 负责租户的生命周期与成员管理：创建（创建者成为 Owner）、凭码加入、
/// 成员花名册与按码查询。与 CHttpHandlers（租户内消息/文件/成员业务）
/// 解耦；路由注册到同一 CHttpRouter。依赖 ITenantService。
class CTenantsController
{
   public:
    // @param pTenants 租户注册表 + 成员（ITenantService，装配层注入）。
    explicit CTenantsController(sc::ITenantService* pTenants);

    // 注册本控制器路由（/api/tenant、/api/tenant/info、/api/tenant/join、/api/tenant/members）。
    void RegisterRoutes(web::CHttpRouter& router);

    // 创建租户：POST /api/tenant（名称为请求体）。创建者成为 Owner。
    bool HandleCreate(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 查询租户：GET /api/tenant/info?code=xxx。
    bool HandleInfo(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 凭码加入（加入当前/指定租户）：POST /api/tenant/join（body 可携带码）。
    bool HandleJoin(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 当前租户成员花名册：GET /api/tenant/members。
    bool HandleMembers(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 当前租户状态（对账）：GET /api/tenant/state（改名/被踢/已删检测）。
    bool HandleState(web::CHttpRequest& req, web::CHttpResponse& resp);

   private:
    sc::ITenantService* m_pTenants;  // 租户注册表 + 成员（生命周期由装配层管理）
};

}  // namespace datahub
