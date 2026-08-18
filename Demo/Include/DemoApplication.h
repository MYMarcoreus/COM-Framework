#pragma once

#include <cstdint>

#include "Application/MyApplication.h"
#include "Config/Config.h"

namespace demo {

// 前置声明，减少头文件依赖。
class DemoService;

/// @brief Demo 服务器应用程序。
///
/// 验证 ServerCore：注册网络组件与协议处理服务，通过 ModuleManager
/// 统一管理日志 / 定时器 / 网络模块的生命周期。
class DemoApplication : public sc::MyApplication
{
public:
    explicit DemoApplication(std::uint16_t port);

    virtual ~DemoApplication();

protected:
    // 注册组件：网络组件与协议处理服务。
    bool RegisterComponents() override;

    // 注册模块：日志 / 定时器 / 网络。
    bool RegisterModules() override;

    // 初始化完成钩子。
    bool OnInitialize() override;

    // 启动完成钩子。
    bool OnStart() override;

    // 关闭钩子。
    void OnShutdown() override;

private:
    std::uint16_t port_;
    common::Config config_;
    DemoService* service_; // 借用指针，由组件管理器持有
};

} // namespace demo
