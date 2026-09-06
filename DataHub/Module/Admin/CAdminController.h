#pragma once

#include <string>

#include "Framework/HttpMessage.h"
#include "Module/Storage/IDataStore.h"
#include "Module/Tenant/ITenantService.h"

namespace datahub {

namespace web {
class CHttpRouter;
}

/// @brief 服务端管理（运维控制面）API 控制器。
///
/// 运行在本机回环专用端口的 /api/admin/* 下，提供跨租户全量管理：
/// 全部租户一览 / 统计、单租户详情（成员 + 数据条目）、成员角色与移除、
/// 数据条目查看与删除、租户改名 / 配额调整 / 删除。与设备侧"我的租户"
/// 自助管理不同：本控制器面向服务器运维，操作不经过 X-Tenant 成员闸门
/// （由 CAdminServerModule 统一做回环访问校验）。
class CAdminController
{
   public:
    // @param pStore   数据存储接口（IDataStore）
    // @param pTenants 租户注册表 + 成员（ITenantService）
    CAdminController(sc::IDataStore* pStore, sc::ITenantService* pTenants);

    // 注册管理路由（/api/admin/*）。
    void RegisterRoutes(web::CHttpRouter& router);

   private:
    // 全部租户一览 + 汇总统计：GET /api/admin/overview。
    bool HandleOverview(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 租户详情（信息 + 成员 + 数据条目）：GET /api/admin/tenant?code=xxx。
    bool HandleTenant(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 删除租户（并清空其数据）：DELETE /api/admin/tenant?code=xxx。
    bool HandleTenantDelete(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 重命名租户：POST /api/admin/tenant/rename（表单 code/name）。
    bool HandleRename(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 调整配额：POST /api/admin/tenant/limits（表单 code/maxItems/maxTotalBytes/maxItemBytes）。
    bool HandleLimits(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 数据条目元信息（文本附内容预览）：GET /api/admin/item?code&id。
    bool HandleItem(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 删除数据条目：DELETE /api/admin/item?code&id。
    bool HandleItemDelete(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 移除成员：DELETE /api/admin/member?code&account。
    bool HandleMemberDelete(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 设置成员角色：POST /api/admin/member/role（表单 code/account/role）。
    bool HandleMemberRole(web::CHttpRequest& req, web::CHttpResponse& resp);

    sc::IDataStore* m_pStore;        // 数据存储（生命周期由装配层管理）
    sc::ITenantService* m_pTenants;  // 租户注册表 + 成员
};

}  // namespace datahub
