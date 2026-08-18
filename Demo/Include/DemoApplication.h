#pragma once

#include <cstdint>

#include "Application/MyApplication.h"
#include "Component/ScopedInterfacePtr.h"
#include "Network/INetwork.h"
#include "Network/INetworkHandler.h"

namespace demo {

// 前置声明，减少头文件依赖。
class DemoService;

/// @brief Demo 服务器应用程序。
///
/// 验证 ServerCore：注册网络组件与协议处理服务，建立两者关联并启动服务器。
class DemoApplication : public sc::MyApplication
{
public:
    explicit DemoApplication(std::uint16_t port);

    virtual ~DemoApplication();

protected:
    // 注册组件。
    bool RegisterComponents() override;

    // 初始化完成钩子：建立组件关联。
    bool OnInitialize() override;

    // 启动钩子：启动 TCP 服务器。
    bool OnStart() override;

    // 关闭钩子：停止网络。
    void OnShutdown() override;

private:
    std::uint16_t port_;
    sc::ScopedInterfacePtr<sc::INetwork> network_;
    sc::ScopedInterfacePtr<sc::INetworkHandler> handler_;
    DemoService* service_; // 借用指针，由组件管理器持有
};

} // namespace demo
