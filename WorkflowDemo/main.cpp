// ============================================================================
// main.cpp —— WorkflowDemo 程序入口
//
// 职责：解析命令行参数，启动 HTTP echo 服务器（实现见 HttpEchoServer）。
//
// 用法:
//   ./build/WorkflowDemo/demo_server <port>   —— 启动 HTTP echo 服务（默认 8888）
// ============================================================================
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "HttpEchoServer.h"

int main(int argc, char* argv[])
{
    unsigned short nPort = 8888;
    if (argc > 1)
        nPort = static_cast<unsigned short>(atoi(argv[1]));

    // 忽略 SIGPIPE（workflow 已处理，避免进程被信号终止）
    signal(SIGPIPE, SIG_IGN);

    demo::CHttpEchoServer server;
    if (server.Start(nPort) == 0)
    {
        printf("WorkflowDemo HTTP echo server listening on %u ... "
               "(Ctrl-C to stop)\n", nPort);
        server.Wait();
        server.Stop();
        return 0;
    }

    perror("server start failed");
    return 1;
}
