// ============================================================================
// main.cpp —— DataHub 程序入口
//
// 流程：main → CDataHubApplication → 模块管理器 → 数据存储 → HTTP 服务（Workflow）
// 用法：./build/debug/datahub [port]   （port 省略时读取 datahub.ini 或默认 8888）
// ============================================================================
#include <signal.h>

#include <cstdint>
#include <cstdlib>

#include "Application/DataHubApplication.h"
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

    // 忽略 SIGPIPE（workflow 已处理，避免进程被信号终止）。
    signal(SIGPIPE, SIG_IGN);

    datahub::CDataHubApplication app(port);

    if (!app.Initialize())
    {
        common::log::CLogger::Instance().Error("DataHub 初始化失败");
        return -1;
    }
    if (!app.Start())
    {
        common::log::CLogger::Instance().Error("DataHub 启动失败（端口可能被占用）");
        app.Shutdown();
        return -1;
    }

    common::log::CLogger::Instance().Info("DataHub 已启动，请在浏览器访问 http://<本机IP>:端口/");

    int result = app.Run();

    app.Shutdown();
    common::log::CLogger::Instance().Info("DataHub 已退出");
    return result;
}
