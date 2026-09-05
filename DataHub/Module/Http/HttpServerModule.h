#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "Framework/HttpRouter.h"
#include "Module/Http/IHttpService.h"
#include "Module/InterfaceMap.h"
#include "Module/Module.h"
#include "Module/ScopedInterfacePtr.h"
#include "Module/Storage/IDataStore.h"
#include "Module/Tenant/ITenantService.h"
#include "Observability/IMetrics.h"
#include "workflow/WFHttpServer.h"

namespace datahub {

using sc::IDataStore;
using sc::IHttpService;

// 前置声明。
class CAdminController;
class CHttpHandlers;
class CMemberService;
class CTenantsController;
class CWebPageController;

/// @brief HTTP 数据传输服务模块（装配层）。
///
/// 职责（三层中的"模块装配层"）：
///   - 生命周期：Initialize（解析 IDataStore + 组装业务层 + 注册路由）/
///     Start / Stop / Shutdown；
///   - 装配：创建 CMemberService / CHttpHandlers / CTenantsController /
///     CWebPageController，注册路由到 web::CHttpRouter；
///   - 请求入口：OnRequest 回调 → 成员记录 → 路由分发 → 未命中回 404。
///
/// 分层：
///   - 框架层（web::，Framework/ 目录）：CHttpRouter 路由注册表、CHttpRequest /
///     CHttpResponse 消息读写（HttpMessage.h）、CHttpText 文本编解码工具
///   - 业务层：CHttpHandlers（租户内消息/文件/成员）、CTenantsController（租户管理）、
///     CMemberService（在线成员）、CWebPageController（页面/静态资源）
///   - 装配层：本类（生命周期 + 路由注册）
///
/// 可观测性（可选依赖 IMetrics，未装配时不上报）：记录 http.requests、
/// http.status.<class>、http.members（当前租户仪表），以及按租户的
/// http.<tenantCode>.requests（规模化观测）；访问日志带 rid 与租户标签。
///
/// 前端页面为独立资源文件，构建时由 Makefile 部署到用户目录
/// `~/.datahub/`（index.html + style.css + app.js），运行时从磁盘读取。
///
/// 模块名 "http"。
class CHttpServerModule : public sc::CModule, public IHttpService
{
   public:
    // 创建 HTTP 服务模块。
    // @param nPort     监听端口。
    // @param strWebDir 前端静态资源目录（含 index.html/style.css/app.js）；
    //                 空串表示默认用户目录 `$HOME/.datahub`。
    explicit CHttpServerModule(std::uint16_t nPort, const std::string& strWebDir = "");

    // 创建 HTTP 服务模块（带单次上传/请求体上限）。
    // @param nMaxBodyBytes 单次上传/请求体上限（字节；0 表示不限制）。
    CHttpServerModule(std::uint16_t nPort, const std::string& strWebDir, std::uint64_t nMaxBodyBytes);

    // 创建 HTTP 服务模块（含内部管理 API 令牌）。
    // @param strAdminToken 管理 API（/api/admin/*）访问令牌；空串 = 不开放管理 API。
    CHttpServerModule(std::uint16_t nPort, const std::string& strWebDir, std::uint64_t nMaxBodyBytes,
                      const std::string& strAdminToken);

    virtual ~CHttpServerModule();

    bool Initialize(const sc::CResolveContext& ctx) override;
    bool Start() override;
    void Stop() override;
    void Shutdown() override;

    std::uint16_t Port() const override;
    std::string Status() const override;

    SC_DECLARE_INTERFACE_MAP();

   private:
    // 请求处理回调（WFHttpServer 线程池中执行；lambda 捕获本实例）。
    // 流程：X-Tenant → CTenant（装入 CRequestContext，挂到请求上下文）→
    //       成员记录 → 分发 → 状态码分类 + 访问日志。
    void OnRequest(WFHttpTask* pServerTask);

    std::uint16_t m_nPort;
    std::string m_strWebDir;        // 前端资源目录
    std::uint64_t m_nMaxBodyBytes;  // 单次上传/请求体上限（字节；0 表示不限制）
    std::string m_strAdminToken;    // 管理 API 令牌（空 = 不开放 /api/admin/*）

    sc::ScopedInterfacePtr<IDataStore> m_pStore;
    sc::ScopedInterfacePtr<sc::ITenantService> m_pTenants;  // 租户注册表（解析 X-Tenant）
    sc::ScopedInterfacePtr<sc::IMetrics> m_pMetrics;        // 指标注册表（可选，缺失不上报）
    std::unique_ptr<CMemberService> m_pMembers;             // 成员服务（业务层）
    std::unique_ptr<CHttpHandlers> m_pHandlers;             // 租户内业务 API（业务层）
    std::unique_ptr<CTenantsController> m_pTenantCtl;       // 租户管理（业务层）
    std::unique_ptr<CWebPageController> m_pPages;           // 页面/静态资源（业务层）
    std::unique_ptr<CAdminController> m_pAdminCtl;          // 管理 API（本机回环 + 令牌）
    web::CHttpRouter m_router;                              // 租户内/页面路由
    web::CHttpRouter m_routerAdmin;                         // 管理 API 路由（/api/admin/*）
    WFHttpServer m_server;
    bool m_bStarted;
};

}  // namespace datahub
