// ============================================================================
// HttpEchoServer.h —— HTTP echo 服务器（封装 WFHttpServer）
//
// 职责：把请求行 + 所有请求头 + 请求体回显给客户端。
// 由 main.cpp 作为入口调用；本类隐藏 workflow 细节。
// ============================================================================
#ifndef WORKFLOWDEMO_HTTPECHOSERVER_H
#define WORKFLOWDEMO_HTTPECHOSERVER_H

#include <string>

#include "workflow/WFHttpServer.h"
#include "workflow/WFFacilities.h"

namespace demo
{

// ----------------------------------------------------------------------------
// CHttpEchoServer：HTTP echo 服务器封装
// ----------------------------------------------------------------------------
class CHttpEchoServer
{
public:
    // 构造：无参（内部创建 WFHttpServer）
    CHttpEchoServer();

    // 启动服务，监听指定端口；成功返回 0，失败返回非 0
    int Start(unsigned short nPort);

    // 阻塞等待服务运行（Ctrl-C 退出后返回）
    void Wait();

    // 停止服务
    void Stop();

private:
    // 请求处理回调（WFHttpServer 调用；static 适配 C 回调签名）
    static void ProcessRequest(WFHttpTask* pServerTask);

    // 打印客户端地址（演示 get_peer_addr）
    static void PrintPeer(WFHttpTask* pServerTask, char* szAddr, size_t nSize,
                          unsigned short* pPort);

    WFHttpServer m_server;                // workflow 服务
    WFFacilities::WaitGroup m_waitGroup;  // 主线程等待（1 个信号）
};

} // namespace demo

#endif // WORKFLOWDEMO_HTTPECHOSERVER_H
