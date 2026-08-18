#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "DemoApplication.h"

/// @brief Demo 服务器入口。
///
/// 流程：main → DemoApplication → 组件管理器 → 网络 → 监听 → 接收 → 协议 → 响应。
int main(int argc, char* argv[])
{
    // 解析端口参数（默认 9000）
    std::uint16_t port = 9000;
    if (argc > 1)
    {
        int value = std::atoi(argv[1]);
        if (value > 0 && value <= 65535)
        {
            port = static_cast<std::uint16_t>(value);
        }
    }

    demo::DemoApplication app(port);

    if (!app.Initialize())
    {
        std::fprintf(stderr, "Demo 初始化失败\n");
        return -1;
    }
    if (!app.Start())
    {
        std::fprintf(stderr, "Demo 启动失败（端口可能被占用）\n");
        app.Shutdown();
        return -1;
    }

    std::printf("Demo 服务器已启动，监听端口 %u，按 Ctrl+C 退出\n", static_cast<unsigned>(port));
    std::fflush(stdout);

    int result = app.Run();

    app.Shutdown();
    std::printf("Demo 服务器已退出\n");
    return result;
}
