// ============================================================================
// WorkflowDemo —— 使用 Sogou Workflow（nossl）的示例程序
//
// 展示 workflow 核心能力：
//   1. HTTP 服务端（WFHttpServer）：echo 请求行 / 请求头 / 请求体
//   2. HTTP 客户端（WFTaskFactory::create_http_task）：拉取指定 URL
//
// 用法:
//   ./build/WorkflowDemo/demo_server <port>          —— 启动 HTTP echo 服务
//   ./build/WorkflowDemo/demo_client <url>           —— 请求指定 URL
//   ./build/WorkflowDemo/demo_parallel <url1> <url2> —— 并行请求（示例见 demo_parallel.cpp）
//
// 注意: workflow 为第三方库，静态库由 ThirdParty/build.sh 独立编译，
//       本示例通过 Makefile 链接 libworkflow.a（nossl 分支，无 OpenSSL 依赖）。
// ============================================================================
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFHttpServer.h"
#include "workflow/WFFacilities.h"

// 全局等待组：主线程等待服务运行（Ctrl-C 退出）
static WFFacilities::WaitGroup wait_group(1);

// ----------------------------------------------------------------------------
// HTTP 服务处理回调：把请求行 + 所有请求头 + 请求体回显给客户端
// ----------------------------------------------------------------------------
void EchoProcess(WFHttpTask* server_task)
{
    protocol::HttpRequest* req = server_task->get_req();
    protocol::HttpResponse* resp = server_task->get_resp();
    protocol::HttpHeaderCursor cursor(req);
    std::string name;
    std::string value;
    std::string body;
    char buf[8192];
    int len;

    // 请求行
    len = snprintf(buf, sizeof(buf), "<p>%s %s %s</p>\n",
                   req->get_method(), req->get_request_uri(), req->get_http_version());
    resp->append_output_body(buf, len);

    // 请求头
    while (cursor.next(name, value))
    {
        len = snprintf(buf, sizeof(buf), "<p>%s: %s</p>\n",
                       name.c_str(), value.c_str());
        resp->append_output_body(buf, len);
    }

    // 请求体（如有）
    const void* req_body = NULL;
    size_t req_body_len = 0;
    if (req->get_parsed_body(&req_body, &req_body_len) && req_body_len > 0)
    {
        body.assign(static_cast<const char*>(req_body), req_body_len);
        len = snprintf(buf, sizeof(buf), "<p>body(%zu): %s</p>\n",
                       req_body_len, body.c_str());
        resp->append_output_body(buf, len);
    }

    // 响应头
    resp->set_http_version("HTTP/1.1");
    resp->set_status_code("200");
    resp->set_reason_phrase("OK");
    resp->add_header_pair("Content-Type", "text/html");
    resp->add_header_pair("Server", "WorkflowDemo");

    // 打印客户端地址（演示 get_peer_addr）
    char addrstr[128] = "Unknown";
    struct sockaddr_storage addr;
    socklen_t l = sizeof(addr);
    unsigned short port = 0;
    server_task->get_peer_addr((struct sockaddr*)&addr, &l);
    if (addr.ss_family == AF_INET)
    {
        struct sockaddr_in* sin = (struct sockaddr_in*)&addr;
        inet_ntop(AF_INET, &sin->sin_addr, addrstr, 128);
        port = ntohs(sin->sin_port);
    }
    else if (addr.ss_family == AF_INET6)
    {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&addr;
        inet_ntop(AF_INET6, &sin6->sin6_addr, addrstr, 128);
        port = ntohs(sin6->sin6_port);
    }
    printf("[echo] peer=%s:%u seq=%lld\n", addrstr, port, server_task->get_task_seq());
}

// ----------------------------------------------------------------------------
// 主函数：启动 HTTP echo 服务器
// ----------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    unsigned short port = 8888;
    if (argc > 1)
        port = (unsigned short)atoi(argv[1]);

    // 忽略 SIGPIPE（workflow 已处理，避免进程被信号终止）
    signal(SIGPIPE, SIG_IGN);

    WFHttpServer server(EchoProcess);
    if (server.start(port) == 0)
    {
        printf("WorkflowDemo HTTP echo server listening on %u ... (Ctrl-C to stop)\n", port);
        wait_group.wait();
        server.stop();
    }
    else
    {
        perror("server start failed");
        return 1;
    }

    return 0;
}
