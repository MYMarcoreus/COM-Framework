#include <cstdint>
#include <cstdlib>

#include "Application/ExampleApplication.h"
#include "Log/Logger.h"

/// @brief ServerExample 服务器入口。
///
/// 流程：main → CExampleApplication → 模块管理器 → 网络 → 监听 → 接收 → 协议 → 响应。
int main(int argc, char* argv[])
{
    // 解析端口参数（0 表示从配置文件读取）
    std::uint16_t port = 0;
    if (argc > 1)
    {
        int value = std::atoi(argv[1]);
        if (value > 0 && value <= 65535)
        {
            port = static_cast<std::uint16_t>(value);
        }
    }

    serverexample::CExampleApplication app(port);

    if (!app.Initialize())
    {
        common::log::CLogger::Instance().Error("ServerExample 初始化失败");
        return -1;
    }
    if (!app.Start())
    {
        common::log::CLogger::Instance().Error("ServerExample 启动失败（端口可能被占用）");
        app.Shutdown();
        return -1;
    }

    int result = app.Run();

    app.Shutdown();
    common::log::CLogger::Instance().Info("ServerExample 服务器已退出");
    return result;
}
