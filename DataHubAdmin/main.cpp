// ============================================================================
// main.cpp —— DataHubAdmin（服务端管理控制面）程序入口
//
// 独立的服务器 exe：承载租户管理网页，并把 /api/admin/* 代理到 DataHub
// 数据面（本机回环 + 令牌）。用法：./datahub-admin [port]
// ============================================================================
#include <signal.h>

#include <cstdint>
#include <cstdlib>

#include "Application/DataHubAdminApplication.h"
#include "Log/Logger.h"

int main(int argc, char* argv[])
{
    // 解析端口参数（0 表示从配置文件读取）。
    std::uint16_t port = 0;
    if (argc > 1)
    {
        int value = std::atoi(argv[1]);
        if (value > 0 && value <= 65535)
        {
            port = static_cast<std::uint16_t>(value);
        }
    }

    signal(SIGPIPE, SIG_IGN);

    datahubadmin::CDataHubAdminApplication app(port);

    if (!app.Initialize())
    {
        common::log::CLogger::Instance().Error("DataHubAdmin 初始化失败");
        return -1;
    }
    if (!app.Start())
    {
        common::log::CLogger::Instance().Error("DataHubAdmin 启动失败（端口可能被占用）");
        app.Shutdown();
        return -1;
    }

    common::log::CLogger::Instance().Info(
        "DataHubAdmin 已启动（管理控制面，仅本机访问）。请打开 http://127.0.0.1:<端口>/");

    int result = app.Run();

    app.Shutdown();
    common::log::CLogger::Instance().Info("DataHubAdmin 已退出");
    return result;
}
