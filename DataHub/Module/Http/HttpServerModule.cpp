#include "Module/Http/HttpServerModule.h"

#include <string>

#include "Framework/HttpText.h"
#include "Log/Logger.h"
#include "Module/Http/HttpHandlers.h"
#include "Module/InterfaceMap.h"
#include "Module/Http/MemberService.h"
#include "Module/ResolveContext.h"
#include "workflow/HttpMessage.h"

namespace datahub {

/// @brief 创建 HTTP 服务模块。
CHttpServerModule::CHttpServerModule(std::uint16_t nPort, const std::string& strWebDir)
    : sc::CModule("http"),
      m_nPort(nPort),
      m_strWebDir(strWebDir),
      m_server([this](WFHttpTask* pTask) { OnRequest(pTask); }),
      m_bStarted(false)
{
    // 依赖 IDataStore 接口模块：生命周期拓扑排序保证其先初始化 / 启动。
    AddDependency(sc::IID_IDataStore());
}

/// @brief 销毁 HTTP 服务模块。
CHttpServerModule::~CHttpServerModule()
{
    Stop();
}

/// @brief 从初始化上下文解析数据存储接口，组装业务层并注册路由。
bool CHttpServerModule::Initialize(const sc::CResolveContext& ctx)
{
    m_pStore.Reset(ctx.Resolve<IDataStore>());
    if (m_pStore == nullptr)
    {
        return false;
    }

    // 业务层实例（依赖注入）。
    m_pMembers = std::unique_ptr<CMemberService>(new CMemberService());
    m_pHandlers = std::unique_ptr<CHttpHandlers>(new CHttpHandlers(m_pStore.Get(), m_pMembers.get()));

    // 加载首页（index.html 内容，供 GET / 返回）。
    if (!LoadIndexHtml())
    {
        common::log::CLogger::Instance().Warn("[DataHub] 前端 index.html 加载失败: " + m_strWebDir +
                                              "（GET / 将返回 503）");
    }

    // 注册路由（框架层 CHttpRouter）。
    // —— 页面 / 静态资源（本模块直接处理）
    m_router.Register({"GET", "/", [this](web::CHttpRequest&, web::CHttpResponse& resp) { return HandleIndex(resp); }});
    m_router.Register({"GET", "/style.css", [this](web::CHttpRequest&, web::CHttpResponse& resp)
    { return HandleStatic(resp, "style.css"); }});
    m_router.Register({"GET", "/app.js",
                       [this](web::CHttpRequest&, web::CHttpResponse& resp) { return HandleStatic(resp, "app.js"); }});

    // —— 业务 API：由业务控制器（CHttpHandlers）自注册，路由归属业务类。
    m_pHandlers->RegisterRoutes(m_router);
    return true;
}

/// @brief 从磁盘加载前端 index.html 文件。
bool CHttpServerModule::LoadIndexHtml()
{
    std::string strDir = m_strWebDir;
    if (strDir.empty())
    {
        const char* szHome = ::getenv("HOME");
        if (szHome == nullptr)
        {
            return false;
        }
        strDir = std::string(szHome) + "/.datahub";
    }
    std::string strContent;
    if (!web::CHttpText::ReadFile(strDir + "/index.html", strContent))
    {
        return false;
    }
    m_strIndexHtml = strContent;
    return !m_strIndexHtml.empty();
}

/// @brief 启动 HTTP 服务。
bool CHttpServerModule::Start()
{
    if (m_bStarted)
    {
        return true;
    }
    if (m_server.start(m_nPort) == 0)
    {
        m_bStarted = true;
        common::log::CLogger::Instance().Info("[DataHub] HTTP 服务已启动，监听端口 " + std::to_string(m_nPort));
        return true;
    }
    return false;
}

/// @brief 停止 HTTP 服务。
void CHttpServerModule::Stop()
{
    if (m_bStarted)
    {
        m_server.stop();
        m_bStarted = false;
        common::log::CLogger::Instance().Info("[DataHub] HTTP 服务已停止");
    }
}

void CHttpServerModule::Shutdown()
{
    Stop();
    m_pHandlers.reset();
    m_pMembers.reset();
    m_pStore.Reset();
}

std::uint16_t CHttpServerModule::Port() const
{
    return m_nPort;
}

std::string CHttpServerModule::Status() const
{
    return "http:port=" + std::to_string(m_nPort) + " started=" + (m_bStarted ? "1" : "0");
}

// ----------------------------------------------------------------------------
// 请求处理回调（Workflow 线程池中执行）
// ----------------------------------------------------------------------------
void CHttpServerModule::OnRequest(WFHttpTask* pServerTask)
{
    // 封装请求 / 响应（业务层不直接接触 workflow 类型）。
    web::CHttpRequest req(pServerTask);
    web::CHttpResponse resp(pServerTask);

    // 记录成员活跃（客户端标识），供在线成员列表展示。
    if (m_pMembers)
    {
        m_pMembers->Touch(req);
    }

    if (!m_router.Dispatch(req, resp))
    {
        resp.WriteText("Not Found", "404", "text/plain");
    }

    // 统一响应头。
    protocol::HttpResponse* pRaw = pServerTask->get_resp();
    pRaw->add_header_pair("Server", "DataHub/1.0");
}

// ----------------------------------------------------------------------------
// 首页与静态资源
// ----------------------------------------------------------------------------
bool CHttpServerModule::HandleIndex(web::CHttpResponse& resp)
{
    if (m_strIndexHtml.empty())
    {
        resp.WriteText("前端页面未加载", "503", "text/plain");
        return true;
    }
    resp.WriteText(m_strIndexHtml, "200", "text/html; charset=utf-8");
    return true;
}

bool CHttpServerModule::HandleStatic(web::CHttpResponse& resp, const std::string& strName)
{
    std::string strDir = m_strWebDir;
    if (strDir.empty())
    {
        const char* szHome = ::getenv("HOME");
        if (szHome == nullptr)
        {
            resp.WriteText("static unavailable", "503", "text/plain");
            return true;
        }
        strDir = std::string(szHome) + "/.datahub";
    }
    std::string strContent;
    if (!web::CHttpText::ReadFile(strDir + "/" + strName, strContent))
    {
        resp.WriteText("not found", "404", "text/plain");
        return true;
    }
    std::string strMime = web::CHttpText::MimeType(strName);
    resp.WriteText(strContent, "200", (strMime + "; charset=utf-8").c_str());
    return true;
}

SC_BEGIN_INTERFACE_MAP(CHttpServerModule, sc::CModule)
SC_INTERFACE_ENTRY(IHttpService)
SC_END_INTERFACE_MAP(CHttpServerModule, sc::CModule)

}  // namespace datahub
