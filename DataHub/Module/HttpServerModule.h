#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "Framework/HttpRouter.h"
#include "Module/IDataStore.h"
#include "Module/IHttpService.h"
#include "Module/InterfaceMap.h"
#include "Module/Module.h"
#include "Module/ScopedInterfacePtr.h"
#include "workflow/WFHttpServer.h"

namespace datahub {

using sc::IDataStore;
using sc::IHttpService;

// 前置声明。
class CHttpHandlers;
class CMemberService;

/// @brief HTTP 数据传输服务模块（装配层）。
///
/// 职责（三层中的"模块装配层"）：
///   - 生命周期：Initialize（解析 IDataStore + 组装业务层 + 注册路由）/
///     Start / Stop / Shutdown；
///   - 装配：创建 CMemberService / CHttpHandlers，注册路由到 web::CHttpRouter；
///   - 请求入口：OnRequest 回调 → 成员记录 → 路由分发 → 未命中回 404。
///
/// 分层：
///   - 框架层（web::）：CHttpRouter 路由注册表、CHttpIo 消息读写、
///     CHttpEncoding 编解码、CHttpMedia 类型判定、CHttpFile 文件收发
///   - 业务层：CHttpHandlers（各 API 业务处理）、CMemberService（在线成员）
///   - 装配层：本类（生命周期 + 路由注册）
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
    void OnRequest(WFHttpTask* pServerTask);

    // 首页 GET /：返回前端 index.html。
    bool HandleIndex(web::CHttpResponse& resp);

    // 静态资源 GET /style.css、/app.js。
    bool HandleStatic(web::CHttpResponse& resp, const std::string& strName);

    // 从磁盘加载 index.html 内容（Start 前调用）。
    bool LoadIndexHtml();

    std::uint16_t m_nPort;
    std::string m_strWebDir;  // 前端资源目录
    std::string m_strIndexHtml;  // 已加载的 index.html 内容（空表示加载失败）

    sc::ScopedInterfacePtr<IDataStore> m_pStore;
    std::unique_ptr<CMemberService> m_pMembers;    // 成员服务（业务层）
    std::unique_ptr<CHttpHandlers> m_pHandlers;    // 业务 API（业务层）
    web::CHttpRouter m_router;                     // 路由注册表（框架层）
    WFHttpServer m_server;
    bool m_bStarted;
};

}  // namespace datahub
