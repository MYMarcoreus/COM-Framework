#pragma once

#include <cstdint>
#include <string>

#include "Framework/HttpMessage.h"
#include "Module/Storage/IDataStore.h"
#include "Module/Tenant/ITenantService.h"

namespace datahub {

// 默认单次上传/请求体上限（字节）：32MB。可由装配层按配置覆盖。
static const std::uint64_t kDefaultMaxBodyBytes = 33554432ULL;

// 前置声明（装配层持有实例）。
class CMemberService;
namespace web {
class CHttpRouter;
}

/// @brief HTTP 业务处理器（DataHub 租户内业务 API）。
///
/// 只处理"租户内"消息/文件/成员业务；"当前租户"由装配层在入口解析进
/// CRequestContext（经 CHttpRequest::UserData 挂载），本控制器经
/// RequestContextOf(req).Tenant() 读取。注入 ITenantService 仅用于授权
/// （删除按角色：Owner 可删任意，Member 仅自删）。租户生命周期管理在
/// CTenantsController；页面/静态资源在 CWebPageController。
class CHttpHandlers
{
   public:
    // @param pStore         数据存储（IDataStore，装配层注入）
    // @param pMembers       成员服务（CMemberService，装配层注入）
    // @param pTenants       租户注册表（仅用于角色授权，装配层注入）
    // @param nMaxBodyBytes  单次上传/请求体上限（字节）；超过返回 413。
    CHttpHandlers(sc::IDataStore* pStore, CMemberService* pMembers, sc::ITenantService* pTenants,
                  std::uint64_t nMaxBodyBytes = kDefaultMaxBodyBytes);

    // 注册本控制器负责的全部业务路由（由装配层在 Initialize 时调用）。
    void RegisterRoutes(web::CHttpRouter& router);

    // 消息列表：GET /api/list（当前租户）。
    bool HandleList(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 在线成员：GET /api/members（当前租户）。
    bool HandleMembers(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 上传文本：POST /api/text（body 为内容）。
    bool HandleUploadText(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 获取文本：GET /api/text/<id>。
    bool HandleGetText(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 上传文件：POST /api/file（header X-File-Name 指定文件名）。
    bool HandleUploadFile(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 下载文件 / 图片：GET /api/file/<id>。
    bool HandleGetFile(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 删除：DELETE /api/item/<id>（归属校验：仅创建者可删）。
    bool HandleDelete(web::CHttpRequest& req, web::CHttpResponse& resp);

   private:
    sc::IDataStore* m_pStore;        // 数据存储（生命周期由装配层管理）
    CMemberService* m_pMembers;      // 成员服务
    sc::ITenantService* m_pTenants;  // 租户注册表（授权：角色判定）
    std::uint64_t m_nMaxBodyBytes;   // 单次上传/请求体上限（字节）
};

}  // namespace datahub
