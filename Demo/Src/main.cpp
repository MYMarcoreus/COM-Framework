#include <cstdint>
#include <cstdlib>

#include "DemoApplication.h"
#include "Log/Logger.h"

/// @brief Demo 服务器入口。
///
/// 流程：main → CDemoApplication → 组件管理器 → 网络 → 监听 → 接收 → 协议 → 响应。
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

    demo::CDemoApplication app(port);

    if (!app.Initialize())
    {
        common::CLogger::Instance().Error("Demo 初始化失败");
        return -1;
    }
    if (!app.Start())
    {
        common::CLogger::Instance().Error("Demo 启动失败（端口可能被占用）");
        app.Shutdown();
        return -1;
    }

    int result = app.Run();

    app.Shutdown();
    common::CLogger::Instance().Info("Demo 服务器已退出");
    return result;
}
