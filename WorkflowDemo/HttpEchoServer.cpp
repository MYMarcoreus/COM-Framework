// ============================================================================
// HttpEchoServer.cpp —— HTTP echo 服务器实现
// ============================================================================
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "HttpEchoServer.h"

#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"

namespace demo
{

// ----------------------------------------------------------------------------
// 构造：初始化 workflow 服务与等待组（1 个信号）
// ----------------------------------------------------------------------------
CHttpEchoServer::CHttpEchoServer()
    : m_server(&CHttpEchoServer::ProcessRequest)
    , m_waitGroup(1)
{
}

// ----------------------------------------------------------------------------
// 启动服务
// ----------------------------------------------------------------------------
int CHttpEchoServer::Start(unsigned short nPort)
{
    return m_server.start(nPort);
}

// ----------------------------------------------------------------------------
// 阻塞等待服务运行（直到 WaitGroup 收到信号，即 Ctrl-C 触发的 done）
// ----------------------------------------------------------------------------
void CHttpEchoServer::Wait()
{
    m_waitGroup.wait();
}

// ----------------------------------------------------------------------------
// 停止服务
// ----------------------------------------------------------------------------
void CHttpEchoServer::Stop()
{
    m_server.stop();
}

// ----------------------------------------------------------------------------
// 请求处理回调：把请求行 + 所有请求头 + 请求体回显给客户端
// ----------------------------------------------------------------------------
void CHttpEchoServer::ProcessRequest(WFHttpTask* pServerTask)
{
    protocol::HttpRequest* pReq = pServerTask->get_req();
    protocol::HttpResponse* pResp = pServerTask->get_resp();
    protocol::HttpHeaderCursor cursor(pReq);
    std::string strName;
    std::string strValue;
    std::string strBody;
    char szBuf[8192];
    int nLen;

    // 请求行
    nLen = snprintf(szBuf, sizeof(szBuf), "<p>%s %s %s</p>\n",
                    pReq->get_method(), pReq->get_request_uri(),
                    pReq->get_http_version());
    pResp->append_output_body(szBuf, nLen);

    // 请求头
    while (cursor.next(strName, strValue))
    {
        nLen = snprintf(szBuf, sizeof(szBuf), "<p>%s: %s</p>\n",
                        strName.c_str(), strValue.c_str());
        pResp->append_output_body(szBuf, nLen);
    }

    // 请求体（如有）
    const void* pBody = NULL;
    size_t nBodyLen = 0;
    if (pReq->get_parsed_body(&pBody, &nBodyLen) && nBodyLen > 0)
    {
        strBody.assign(static_cast<const char*>(pBody), nBodyLen);
        nLen = snprintf(szBuf, sizeof(szBuf), "<p>body(%zu): %s</p>\n",
                        nBodyLen, strBody.c_str());
        pResp->append_output_body(szBuf, nLen);
    }

    // 响应头
    pResp->set_http_version("HTTP/1.1");
    pResp->set_status_code("200");
    pResp->set_reason_phrase("OK");
    pResp->add_header_pair("Content-Type", "text/html");
    pResp->add_header_pair("Server", "WorkflowDemo");

    // 打印客户端地址（演示 get_peer_addr）
    char szAddr[128] = "Unknown";
    unsigned short nPort = 0;
    PrintPeer(pServerTask, szAddr, sizeof(szAddr), &nPort);
    printf("[echo] peer=%s:%u seq=%lld\n", szAddr, nPort,
           pServerTask->get_task_seq());
}

// ----------------------------------------------------------------------------
// 打印客户端地址
// ----------------------------------------------------------------------------
void CHttpEchoServer::PrintPeer(WFHttpTask* pServerTask, char* szAddr,
                                size_t nSize, unsigned short* pPort)
{
    struct sockaddr_storage addr;
    socklen_t nLen = sizeof(addr);

    if (pServerTask->get_peer_addr((struct sockaddr*)&addr, &nLen) == 0)
    {
        if (addr.ss_family == AF_INET)
        {
            struct sockaddr_in* pSin = (struct sockaddr_in*)&addr;
            inet_ntop(AF_INET, &pSin->sin_addr, szAddr, nSize);
            *pPort = ntohs(pSin->sin_port);
        }
        else if (addr.ss_family == AF_INET6)
        {
            struct sockaddr_in6* pSin6 = (struct sockaddr_in6*)&addr;
            inet_ntop(AF_INET6, &pSin6->sin6_addr, szAddr, nSize);
            *pPort = ntohs(pSin6->sin6_port);
        }
    }
}

} // namespace demo
