#include "Module/AdminConsoleModule.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include "Log/Logger.h"
#include "Module/InterfaceMap.h"
#include "Module/ResolveContext.h"
#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFTaskFactory.h"
#include "workflow/Workflow.h"

namespace datahubadmin {

namespace {

// 是否本机回环来源（管理面仅对本机开放）。
bool IsLoopbackPeer(WFHttpTask* pTask)
{
    struct sockaddr_storage addr;
    socklen_t nLen = sizeof(addr);
    if (pTask->get_peer_addr(reinterpret_cast<struct sockaddr*>(&addr), &nLen) != 0)
    {
        return false;
    }
    char szIp[INET6_ADDRSTRLEN] = "";
    if (addr.ss_family == AF_INET)
    {
        struct sockaddr_in* pSin = reinterpret_cast<struct sockaddr_in*>(&addr);
        if (inet_ntop(AF_INET, &pSin->sin_addr, szIp, sizeof(szIp)) == nullptr)
        {
            return false;
        }
    }
    else if (addr.ss_family == AF_INET6)
    {
        struct sockaddr_in6* pSin6 = reinterpret_cast<struct sockaddr_in6*>(&addr);
        if (inet_ntop(AF_INET6, &pSin6->sin6_addr, szIp, sizeof(szIp)) == nullptr)
        {
            return false;
        }
    }
    else
    {
        return false;
    }
    const std::string strIp = szIp;
    return strIp == "127.0.0.1" || strIp == "::1";
}

}  // namespace

/// @brief 创建管理控制台模块。
CAdminConsoleModule::CAdminConsoleModule(std::uint16_t nPort, const std::string& strUpstream,
                                         const std::string& strToken, const std::string& strWebDir)
    : sc::CModule("adminconsole"),
      m_nPort(nPort),
      m_strUpstream(strUpstream),
      m_strToken(strToken),
      m_strWebDir(strWebDir),
      m_server([this](WFHttpTask* pTask) { OnRequest(pTask); }),
      m_bStarted(false)
{
    // 上游基址归一化：去掉结尾 '/'（拼接时统一加）。
    while (!m_strUpstream.empty() && m_strUpstream[m_strUpstream.size() - 1] == '/')
    {
        m_strUpstream.erase(m_strUpstream.size() - 1);
    }
    // 管理网页目录：空串回退到默认用户目录 $HOME/.datahub-admin。
    if (m_strWebDir.empty())
    {
        const char* szHome = ::getenv("HOME");
        if (szHome != nullptr)
        {
            m_strWebDir = std::string(szHome) + "/.datahub-admin";
        }
    }
}

/// @brief 销毁模块。
CAdminConsoleModule::~CAdminConsoleModule()
{
    Stop();
}

/// @brief 读取管理网页资源（index.html / admin.js）到内存。
bool CAdminConsoleModule::Initialize(const sc::CResolveContext& ctx)
{
    (void)ctx;
    m_indexHtml = "";
    m_adminJs = "";
    if (m_strWebDir.empty())
    {
        return true;
    }
    auto LoadFile = [this](const std::string& strName) -> std::string
    {
        std::string strOut;
        FILE* pFile = std::fopen((m_strWebDir + "/" + strName).c_str(), "rb");
        if (pFile == nullptr)
        {
            return strOut;
        }
        std::fseek(pFile, 0, SEEK_END);
        const long nSize = std::ftell(pFile);
        std::fseek(pFile, 0, SEEK_SET);
        if (nSize > 0)
        {
            strOut.resize(static_cast<std::size_t>(nSize));
            const std::size_t nRead = std::fread(&strOut[0], 1, static_cast<std::size_t>(nSize), pFile);
            strOut.resize(nRead);
        }
        std::fclose(pFile);
        return strOut;
    };
    m_indexHtml = LoadFile("index.html");
    m_adminJs = LoadFile("admin.js");
    if (m_indexHtml.empty())
    {
        common::log::CLogger::Instance().Warn("[DataHubAdmin] 管理网页 index.html 未找到: " + m_strWebDir);
    }
    return true;
}

bool CAdminConsoleModule::Start()
{
    if (m_bStarted)
    {
        return true;
    }
    if (m_server.start(m_nPort) == 0)
    {
        m_bStarted = true;
        common::log::CLogger::Instance().Info("[DataHubAdmin] 管理控制台已启动，监听端口 " + std::to_string(m_nPort) +
                                              "（仅本机回环可访问）");
        return true;
    }
    return false;
}

void CAdminConsoleModule::Stop()
{
    if (m_bStarted)
    {
        m_server.stop();
        m_bStarted = false;
        common::log::CLogger::Instance().Info("[DataHubAdmin] 管理控制台已停止");
    }
}

void CAdminConsoleModule::Shutdown()
{
    Stop();
}

/// @brief 请求分发：本机回环校验 → 静态页 or 代理。
void CAdminConsoleModule::OnRequest(WFHttpTask* pServerTask)
{
    protocol::HttpRequest* pReq = pServerTask->get_req();
    protocol::HttpResponse* pResp = pServerTask->get_resp();
    const char* szUri = pReq->get_request_uri();
    const std::string strUri = szUri != nullptr ? szUri : "/";
    // 仅本机回环（管理面不暴露给局域网设备）。
    if (!IsLoopbackPeer(pServerTask))
    {
        pResp->set_status_code("403");
        pResp->append_output_body_nocopy("{\"error\":\"loopback only\"}", 24);
        return;
    }
    // 路径（去 query）用于区分页面与代理。
    std::string strPath = strUri;
    const std::string::size_type nQuery = strPath.find('?');
    if (nQuery != std::string::npos)
    {
        strPath = strPath.substr(0, nQuery);
    }
    const bool bApi = strPath == "/api/admin" || strPath.compare(0, 11, "/api/admin/") == 0;
    if (bApi)
    {
        ProxyToUpstream(pServerTask, strUri);
    }
    else
    {
        ServePage(pServerTask, strPath);
    }
}

/// @brief 静态管理页（index.html 内联样式 + admin.js）。
void CAdminConsoleModule::ServePage(WFHttpTask* pServerTask, const std::string& strPath)
{
    protocol::HttpResponse* pResp = pServerTask->get_resp();
    if (strPath == "/" || strPath.empty() || strPath == "/index.html")
    {
        if (m_indexHtml.empty())
        {
            pResp->set_status_code("503");
            pResp->append_output_body_nocopy("admin page not loaded", 21);
            return;
        }
        pResp->add_header_pair("Content-Type", "text/html; charset=utf-8");
        pResp->append_output_body_nocopy(m_indexHtml.data(), m_indexHtml.size());
        return;
    }
    if (strPath == "/admin.js")
    {
        if (m_adminJs.empty())
        {
            pResp->set_status_code("503");
            pResp->append_output_body_nocopy("admin.js not loaded", 19);
            return;
        }
        pResp->add_header_pair("Content-Type", "text/javascript; charset=utf-8");
        pResp->append_output_body_nocopy(m_adminJs.data(), m_adminJs.size());
        return;
    }
    pResp->set_status_code("404");
    pResp->append_output_body_nocopy("not found", 9);
}

/// @brief 反向代理 /api/admin/* → DataHub 数据面。
void CAdminConsoleModule::ProxyToUpstream(WFHttpTask* pServerTask, const std::string& strUri)
{
    protocol::HttpRequest* pReq = pServerTask->get_req();
    if (m_strUpstream.empty() || m_strToken.empty())
    {
        protocol::HttpResponse* pResp = pServerTask->get_resp();
        pResp->set_status_code("503");
        pResp->append_output_body_nocopy("{\"error\":\"upstream not configured\"}", 32);
        return;
    }
    const std::string strUrl = m_strUpstream + strUri;

    // 创建上游请求；回调中把响应搬回控制台（series 追加，确保先取数后回包）。
    WFHttpTask* pClient = WFTaskFactory::create_http_task(
        strUrl, 0, 0, [this, pServerTask](WFHttpTask* pDone) { OnUpstreamReply(pDone, pServerTask); });

    protocol::HttpRequest* pClientReq = pClient->get_req();
    pClientReq->set_method(pReq->get_method());
    // 复制请求头（跳过由本层/Workflow 管理的 Host、Content-Length 等）。
    protocol::HttpHeaderCursor cursor(pReq);
    std::string strName;
    std::string strValue;
    while (cursor.next(strName, strValue))
    {
        if (strName == "Host" || strName == "Content-Length" || strName == "Transfer-Encoding" ||
            strName == "Connection" || strName == "Accept-Encoding")
        {
            continue;
        }
        pClientReq->add_header_pair(strName.c_str(), strValue.c_str());
    }
    // 令牌：只存在于控制面服务器配置，浏览器不可见。
    pClientReq->add_header_pair("X-Admin-Token", m_strToken.c_str());
    // 复制请求体（POST 表单等）。
    const void* pBody = nullptr;
    std::size_t nBodyLen = 0;
    if (pReq->get_parsed_body(&pBody, &nBodyLen) && nBodyLen > 0)
    {
        pClientReq->append_output_body_nocopy(pBody, nBodyLen);
    }

    // 挂到本服务器任务的 series：上游完成后才回包。
    series_of(pServerTask)->push_back(pClient);
}

/// @brief 上游响应回调：状态/头/体搬回控制台响应。
void CAdminConsoleModule::OnUpstreamReply(WFHttpTask* pClientTask, WFHttpTask* pServerTask)
{
    protocol::HttpResponse* pServerResp = pServerTask->get_resp();
    if (pClientTask->get_state() == WFT_STATE_SUCCESS)
    {
        protocol::HttpResponse* pClientResp = pClientTask->get_resp();
        const void* pBody = nullptr;
        std::size_t nBodyLen = 0;
        pClientResp->get_parsed_body(&pBody, &nBodyLen);
        if (nBodyLen > 0)
        {
            pClientResp->append_output_body_nocopy(pBody, nBodyLen);
        }
        *pServerResp = std::move(*pClientResp);  // 整包搬移（状态/头/体）
    }
    else
    {
        pServerResp->set_status_code("502");
        pServerResp->append_output_body_nocopy("{\"error\":\"upstream unreachable\"}", 30);
    }
}

SC_BEGIN_INTERFACE_MAP(CAdminConsoleModule, sc::CModule)
SC_END_INTERFACE_MAP(CAdminConsoleModule, sc::CModule)

}  // namespace datahubadmin
