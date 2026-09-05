#include "Module/Http/HttpServerModule.h"

#include <cstdlib>
#include <exception>
#include <string>

#include "Framework/HttpText.h"
#include "Log/Logger.h"
#include "Module/Http/HttpHandlers.h"
#include "Module/Http/MemberService.h"
#include "Module/InterfaceMap.h"
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

    // 前端目录归一化：空串回退到默认用户目录 $HOME/.datahub（仅解析一次）。
    // 后续 LoadIndexHtml / HandleStatic 直接使用 m_strWebDir，不再重复解析。
    if (m_strWebDir.empty())
    {
        const char* szHome = ::getenv("HOME");
        if (szHome != nullptr)
        {
            m_strWebDir = std::string(szHome) + "/.datahub";
        }
    }
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

    // 可观测性：可选依赖 IMetrics（未装配时不上报，不影响启动）。
    m_pMetrics.Reset(ctx.Resolve<sc::IMetrics>());

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
    // m_strWebDir 已在构造时归一化；仍为空表示 HOME 不可用。
    if (m_strWebDir.empty())
    {
        return false;
    }
    std::string strContent;
    if (!web::CHttpText::ReadFile(m_strWebDir + "/index.html", strContent))
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
    if (m_pMetrics != nullptr)
    {
        m_pMetrics->Inc("http.requests");
    }

    // 封装请求 / 响应（业务层不直接接触 workflow 类型）。
    web::CHttpRequest req(pServerTask);
    web::CHttpResponse resp(pServerTask);

    // 业务处理：成员记录 + 路由分发。异常兜底（任何 handler 抛异常都不得
    // 穿过 Workflow 线程），统一回 500 并记录日志。
    try
    {
        if (m_pMembers)
        {
            m_pMembers->Touch(req);
        }
        if (m_pMetrics != nullptr)
        {
            m_pMetrics->SetGauge("http.members", static_cast<double>(m_pMembers != nullptr ? m_pMembers->Count() : 0));
        }
        if (!m_router.Dispatch(req, resp))
        {
            resp.WriteText("Not Found", "404", "text/plain");
        }
    }
    catch (const std::exception& e)
    {
        common::log::CLogger::Instance().Error("[DataHub] 请求处理异常: " + std::string(e.what()));
        resp.WriteText("Internal Server Error", "500", "text/plain");
    }
    catch (...)
    {
        common::log::CLogger::Instance().Error("[DataHub] 请求处理未知异常");
        resp.WriteText("Internal Server Error", "500", "text/plain");
    }

    // 按状态码分布记录（供错误率 / 可用性观测）。
    if (m_pMetrics != nullptr)
    {
        std::string strStatus = resp.StatusCode();
        std::string strClass = "5xx";
        if (!strStatus.empty())
        {
            char cFirst = strStatus[0];
            if (cFirst == '2')
                strClass = "2xx";
            else if (cFirst == '3')
                strClass = "3xx";
            else if (cFirst == '4')
                strClass = "4xx";
        }
        m_pMetrics->Inc("http.status." + strClass);
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
    // m_strWebDir 已在构造时归一化；仍为空表示 HOME 不可用。
    if (m_strWebDir.empty())
    {
        resp.WriteText("static unavailable", "503", "text/plain");
        return true;
    }
    std::string strContent;
    if (!web::CHttpText::ReadFile(m_strWebDir + "/" + strName, strContent))
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
