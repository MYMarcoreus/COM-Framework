#include "Module/HttpServerModule.h"

#include "Framework/HttpUtil.h"
#include "Log/Logger.h"
#include "Module/HttpHandlers.h"
#include "Module/InterfaceMap.h"
#include "Module/MemberService.h"
#include "Module/ResolveContext.h"
#include "workflow/HttpMessage.h"

namespace datahub {

namespace {

// 去掉路径中的 query 部分（? 之后）。
std::string StripQuery(const std::string& strUri)
{
    std::string::size_type nQ = strUri.find('?');
    return nQ == std::string::npos ? strUri : strUri.substr(0, nQ);
}

}  // namespace

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
    m_router.Register({"GET", "/", true, [this](WFHttpTask* t, const std::string&) { return HandleIndex(t); }});
    m_router.Register({"GET", "/style.css", true, [this](WFHttpTask* t, const std::string&) {
                           return HandleStatic(t, "style.css");
                       }});
    m_router.Register({"GET", "/app.js", true, [this](WFHttpTask* t, const std::string&) {
                           return HandleStatic(t, "app.js");
                       }});

    // —— 业务 API（委托 CHttpHandlers）
    m_router.Register({"GET", "/api/list", true, [this](WFHttpTask* t, const std::string&) {
                           return m_pHandlers->HandleList(t);
                       }});
    m_router.Register({"GET", "/api/members", true, [this](WFHttpTask* t, const std::string&) {
                           return m_pHandlers->HandleMembers(t);
                       }});
    m_router.Register({"POST", "/api/text", true, [this](WFHttpTask* t, const std::string&) {
                           return m_pHandlers->HandleUploadText(t);
                       }});
    m_router.Register({"GET", "/api/text/", false, [this](WFHttpTask* t, const std::string& p) {
                           return m_pHandlers->HandleGetText(t, web::CHttpUtil::UrlDecode(p.substr(10)));
                       }});
    m_router.Register({"POST", "/api/file", true, [this](WFHttpTask* t, const std::string&) {
                           return m_pHandlers->HandleUploadFile(t);
                       }});
    m_router.Register({"GET", "/api/file/", false, [this](WFHttpTask* t, const std::string& p) {
                           return m_pHandlers->HandleGetFile(t, web::CHttpUtil::UrlDecode(p.substr(10)));
                       }});
    m_router.Register({"DELETE", "/api/item/", false, [this](WFHttpTask* t, const std::string& p) {
                           return m_pHandlers->HandleDelete(t, web::CHttpUtil::UrlDecode(p.substr(10)));
                       }});
    return true;
}

/// @brief 从磁盘加载前端 index.html 文件。
///
/// 路径解析：m_strWebDir 指定（配置 [web] index 的目录）；否则
/// 默认用户目录 `$HOME/.datahub/index.html`（构建时由 Makefile 部署）。
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
    if (!web::CHttpUtil::ReadFile(strDir + "/index.html", strContent))
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
    protocol::HttpRequest* pReq = pServerTask->get_req();
    protocol::HttpResponse* pResp = pServerTask->get_resp();

    // 记录成员活跃（客户端标识），供在线成员列表展示。
    if (m_pMembers)
    {
        m_pMembers->Touch(pServerTask);
    }

    std::string strMethod = pReq->get_method();
    std::string strPath = StripQuery(pReq->get_request_uri());

    if (!m_router.Dispatch(pServerTask, strMethod, strPath))
    {
        web::CHttpUtil::WriteText(pServerTask, "Not Found", "404", "text/plain");
    }

    // 统一响应头。
    pResp->add_header_pair("Server", "DataHub/1.0");
}

// ----------------------------------------------------------------------------
// 首页与静态资源
// ----------------------------------------------------------------------------
bool CHttpServerModule::HandleIndex(WFHttpTask* pServerTask)
{
    protocol::HttpResponse* pResp = pServerTask->get_resp();
    if (m_strIndexHtml.empty())
    {
        pResp->set_status_code("503");
        pResp->add_header_pair("Content-Type", "text/plain; charset=utf-8");
        pResp->append_output_body("前端页面未加载");
        return true;
    }
    pResp->set_status_code("200");
    pResp->add_header_pair("Content-Type", "text/html; charset=utf-8");
    pResp->append_output_body(m_strIndexHtml.data(), m_strIndexHtml.size());
    return true;
}

bool CHttpServerModule::HandleStatic(WFHttpTask* pServerTask, const std::string& strName)
{
    std::string strDir = m_strWebDir;
    if (strDir.empty())
    {
        const char* szHome = ::getenv("HOME");
        if (szHome == nullptr)
        {
            web::CHttpUtil::WriteText(pServerTask, "static unavailable", "503", "text/plain");
            return true;
        }
        strDir = std::string(szHome) + "/.datahub";
    }
    std::string strContent;
    if (!web::CHttpUtil::ReadFile(strDir + "/" + strName, strContent))
    {
        web::CHttpUtil::WriteText(pServerTask, "not found", "404", "text/plain");
        return true;
    }
    protocol::HttpResponse* pResp = pServerTask->get_resp();
    pResp->set_status_code("200");
    std::string strMime = web::CHttpUtil::MimeType(strName);
    pResp->add_header_pair("Content-Type", (strMime + "; charset=utf-8").c_str());
    pResp->append_output_body(strContent.data(), strContent.size());
    return true;
}

SC_BEGIN_INTERFACE_MAP(CHttpServerModule, sc::CModule)
SC_INTERFACE_ENTRY(IHttpService)
SC_END_INTERFACE_MAP(CHttpServerModule, sc::CModule)

}  // namespace datahub
