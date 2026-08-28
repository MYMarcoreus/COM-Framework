#include <cstdint>
#include <cstdlib>

#include "Log/Logger.h"
#include "Application/LogServerApplication.h"

/// @brief LogServer 服务器入口。
///
/// 流程：main → CLogServerApplication → 模块管理器 → 网络 → 监听 → 接收 → 协议 → 落盘。
int main(int argc, char* argv[])
{
    // 解析端口参数（0 表示从配置文件读取）
    std::uint16_t nPort = 0;
    if (argc > 1)
    {
        int nValue = std::atoi(argv[1]);
        if (nValue > 0 && nValue <= 65535)
        {
            nPort = static_cast<std::uint16_t>(nValue);
        }
    }

    logserver::CLogServerApplication app(nPort);

    if (!app.Initialize())
    {
        common::log::CLogger::Instance().Error("LogServer 初始化失败");
        return -1;
    }
    if (!app.Start())
    {
        common::log::CLogger::Instance().Error("LogServer 启动失败（端口可能被占用）");
        app.Shutdown();
        return -1;
    }

    int nResult = app.Run();

    app.Shutdown();
    common::log::CLogger::Instance().Info("LogServer 服务器已退出");
    return nResult;
}
