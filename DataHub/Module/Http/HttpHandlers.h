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
namespace web { class CHttpRouter; }

/// @brief HTTP 业务处理器（DataHub 业务 API，按租户隔离）。
///
/// 实例类：构造时注入数据存储、成员服务与租户注册表，不依赖静态全局状态。
/// 每个请求的"当前租户"由装配层在入口解析后挂到请求上下文（CHttpRequest
/// 的 UserData 槽），业务各方法经 RequestTenant(req) 读取，不再各自解析头。
/// 首页与静态资源由装配层（CWebPageController）处理，不在本类职责内。
class CHttpHandlers
{
   public:
    // @param pStore         数据存储（IDataStore，装配层注入）
    // @param pMembers       成员服务（CMemberService，装配层注入）
    // @param pTenants       租户注册表（ITenantService，装配层注入）
    // @param nMaxBodyBytes  单次上传/请求体上限（字节）；超过返回 413。
    CHttpHandlers(sc::IDataStore* pStore, CMemberService* pMembers, sc::ITenantService* pTenants,
                  std::uint64_t nMaxBodyBytes = kDefaultMaxBodyBytes);

    // 注册本控制器负责的全部业务路由（由装配层在 Initialize 时调用）。
    void RegisterRoutes(web::CHttpRouter& router);

    // —— 消息 / 文件 / 成员（均按当前租户隔离）——
    bool HandleList(web::CHttpRequest& req, web::CHttpResponse& resp);
    bool HandleMembers(web::CHttpRequest& req, web::CHttpResponse& resp);
    bool HandleUploadText(web::CHttpRequest& req, web::CHttpResponse& resp);
    bool HandleGetText(web::CHttpRequest& req, web::CHttpResponse& resp);
    bool HandleUploadFile(web::CHttpRequest& req, web::CHttpResponse& resp);
    bool HandleGetFile(web::CHttpRequest& req, web::CHttpResponse& resp);
    bool HandleDelete(web::CHttpRequest& req, web::CHttpResponse& resp);

    // —— 空间（租户）——
    // 创建空间：POST /api/space（名称经 X-Space-Name 头传入）。
    bool HandleCreateSpace(web::CHttpRequest& req, web::CHttpResponse& resp);
    // 查询空间：GET /api/space/info?code=xxx（供凭码加入前校验存在性）。
    bool HandleSpaceInfo(web::CHttpRequest& req, web::CHttpResponse& resp);

   private:
    sc::IDataStore* m_pStore;       // 数据存储（生命周期由装配层管理）
    CMemberService* m_pMembers;     // 成员服务
    sc::ITenantService* m_pTenants; // 租户注册表
    std::uint64_t m_nMaxBodyBytes;  // 单次上传/请求体上限（字节）
};

}  // namespace datahub
