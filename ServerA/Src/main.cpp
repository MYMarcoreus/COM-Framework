#include <cstdint>
#include <cstdlib>

#include "Log/Logger.h"
#include "ServerApplication.h"

/// @brief ServerA 服务器入口。
///
/// 复用 ServerCore 骨架：main → ServerApplication → 组件/模块 → 网络 → 回显。
int main(int argc, char* argv[])
{
    // 解析端口参数（默认 9100）
    std::uint16_t port = 9100;
    if (argc > 1)
    {
        int value = std::atoi(argv[1]);
        if (value > 0 && value <= 65535)
        {
            port = static_cast<std::uint16_t>(value);
        }
    }

    servera::ServerApplication app(port);

    if (!app.Initialize())
    {
        common::Logger::Instance().Error("ServerA 初始化失败");
        return -1;
    }
    if (!app.Start())
    {
        common::Logger::Instance().Error("ServerA 启动失败（端口可能被占用）");
        app.Shutdown();
        return -1;
    }

    int result = app.Run();

    app.Shutdown();
    common::Logger::Instance().Info("ServerA 已退出");
    return result;
}
