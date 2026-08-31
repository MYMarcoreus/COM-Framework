#include "Module/HttpServerModule.h"

#include <fstream>
#include <sstream>
#include <string>

#include "Log/Logger.h"
#include "Module/HttpHandlers.h"
#include "Module/HttpUtil.h"
#include "Module/InterfaceMap.h"
#include "Module/MemberTracker.h"
#include "Module/ResolveContext.h"
#include "workflow/HttpMessage.h"

namespace datahub {

IDataStore* CHttpServerModule::s_pStore = nullptr;
const std::string* CHttpServerModule::s_pIndexHtml = nullptr;

/// @brief 创建 HTTP 服务模块。
CHttpServerModule::CHttpServerModule(std::uint16_t nPort, const std::string& strIndex)
    : sc::CModule("http"),
      m_nPort(nPort),
      m_strIndexPath(strIndex),
      m_server(&CHttpServerModule::ProcessRequest),
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

/// @brief 从初始化上下文解析数据存储接口，并加载前端页面。
///
/// @param ctx 初始化上下文（依赖注入）。
///
/// @return true 数据存储接口就绪；false 缺失。
bool CHttpServerModule::Initialize(const sc::CResolveContext& ctx)
{
    m_pStore.Reset(ctx.Resolve<IDataStore>());
    if (m_pStore == nullptr)
    {
        return false;
    }
    // 回调为静态方法，通过静态指针访问数据存储与前端页面。
    s_pStore = m_pStore.Get();
    HttpHandlers::SetStore(s_pStore);

    // 加载前端页面（独立资源文件）；失败仅告警，不影响服务启动。
    if (!LoadIndexHtml())
    {
        common::log::CLogger::Instance().Warn("[DataHub] 前端页面加载失败: " + m_strIndexPath + "（GET / 将返回 503）");
    }
    s_pIndexHtml = &m_strIndexHtml;
    HttpHandlers::SetIndexHtml(s_pIndexHtml);
    return true;
}

/// @brief 从磁盘加载前端页面文件。
///
/// 路径解析：
///   - 配置 [web] index 指定（m_strIndexPath，绝对路径）；否则
///   - 默认用户目录 `$HOME/.datahub/index.html`（构建时由 Makefile 部署）。
/// 直接读取，无自动推导，与进程工作目录无关。
///
/// @return true 加载成功；false 文件不存在或读取失败。
bool CHttpServerModule::LoadIndexHtml()
{
    std::string strPath = m_strIndexPath;
    if (strPath.empty())
    {
        const char* szHome = ::getenv("HOME");
        if (szHome == nullptr)
        {
            return false;
        }
        strPath = std::string(szHome) + "/.datahub/index.html";
    }

    std::ifstream ifs(strPath.c_str(), std::ios::binary);
    if (!ifs.is_open())
    {
        return false;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    m_strIndexHtml = ss.str();
    return !m_strIndexHtml.empty();
}

/// @brief 启动 HTTP 服务。
///
/// @return true 启动成功；false 端口被占用等。
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
    m_pStore.Reset();
    s_pStore = nullptr;
    s_pIndexHtml = nullptr;
    HttpHandlers::SetStore(nullptr);
    HttpHandlers::SetIndexHtml(nullptr);
    MemberTracker::Clear();
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
void CHttpServerModule::ProcessRequest(WFHttpTask* pServerTask)
{
    protocol::HttpRequest* pReq = pServerTask->get_req();
    protocol::HttpResponse* pResp = pServerTask->get_resp();

    // 记录成员活跃（客户端标识），供在线成员列表展示。
    MemberTracker::Touch(pServerTask);

    std::string strMethod = pReq->get_method();
    std::string strPath = pReq->get_request_uri();
    // 去掉 query 部分（? 之后）。
    std::string::size_type nQ = strPath.find('?');
    if (nQ != std::string::npos)
    {
        strPath = strPath.substr(0, nQ);
    }

    if (!Dispatch(pServerTask, strMethod, strPath))
    {
        HttpUtil::WriteText(pServerTask, "Not Found", "404", "text/plain");
    }

    // 统一响应头。
    pResp->add_header_pair("Server", "DataHub/1.0");
}

// ----------------------------------------------------------------------------
// 路由分发
// ----------------------------------------------------------------------------
bool CHttpServerModule::Dispatch(WFHttpTask* pServerTask, const std::string& strMethod, const std::string& strPath)
{
    // 首页（GET /）
    if (strMethod == "GET" && strPath == "/")
    {
        return HttpHandlers::HandleIndex(pServerTask);
    }
    // 前端静态资源：style.css / app.js（index.html 引用的独立文件）
    if (strMethod == "GET" && (strPath == "/style.css" || strPath == "/app.js"))
    {
        return HttpHandlers::HandleStatic(pServerTask, strPath.substr(1));
    }
    // 列表（GET /api/list）
    if (strMethod == "GET" && strPath == "/api/list")
    {
        return HttpHandlers::HandleList(pServerTask);
    }
    // 在线成员（GET /api/members）
    if (strMethod == "GET" && strPath == "/api/members")
    {
        return HttpHandlers::HandleMembers(pServerTask);
    }
    // 上传文本（POST /api/text）
    if (strMethod == "POST" && strPath == "/api/text")
    {
        return HttpHandlers::HandleUploadText(pServerTask);
    }
    // 上传文件（POST /api/file）
    if (strMethod == "POST" && strPath == "/api/file")
    {
        return HttpHandlers::HandleUploadFile(pServerTask);
    }
    // GET /api/text/<id>、GET /api/file/<id>、DELETE /api/item/<id>
    // 注意前缀 "/api/text/" 长度为 10（/api/ 5 + text/ 5）。
    if (strMethod == "GET" && strPath.compare(0, 10, "/api/text/") == 0)
    {
        return HttpHandlers::HandleGetText(pServerTask, HttpUtil::UrlDecode(strPath.substr(10)));
    }
    if (strMethod == "GET" && strPath.compare(0, 10, "/api/file/") == 0)
    {
        return HttpHandlers::HandleGetFile(pServerTask, HttpUtil::UrlDecode(strPath.substr(10)));
    }
    if (strMethod == "DELETE" && strPath.compare(0, 10, "/api/item/") == 0)
    {
        return HttpHandlers::HandleDelete(pServerTask, HttpUtil::UrlDecode(strPath.substr(10)));
    }
    return false;
}

SC_BEGIN_INTERFACE_MAP(CHttpServerModule, sc::CModule)
SC_INTERFACE_ENTRY(IHttpService)
SC_END_INTERFACE_MAP(CHttpServerModule, sc::CModule)

}  // namespace datahub
