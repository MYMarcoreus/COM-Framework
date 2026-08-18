#pragma once

#include <atomic>

#include "Component/ComponentManager.h"

namespace sc {

/// @brief 服务器应用程序基础类。
///
/// 提供统一的初始化、启动、运行、停止和关闭生命周期。
/// 通过 ComponentManager 组合 ServerCore 基础组件，不硬编码具体实现。
///
/// @note 派生类通过重写虚函数扩展服务器生命周期与组件装配。
class MyApplication
{
public:
    MyApplication();

    virtual ~MyApplication();

    // 初始化应用程序。
    virtual bool Initialize();

    // 启动应用程序。
    virtual bool Start();

    // 运行应用程序主循环。
    virtual int Run();

    // 停止应用程序。
    virtual void Stop();

    // 关闭应用程序并释放资源。
    virtual void Shutdown();

    // 获取组件管理器。
    ComponentManager& GetComponentManager();

protected:
    // 注册应用程序需要的基础组件。
    virtual bool RegisterComponents();

    // 初始化完成钩子。
    virtual bool OnInitialize();

    // 启动完成钩子。
    virtual bool OnStart();

    // 主循环钩子，默认等待停止信号。
    virtual int OnRun();

    // 停止钩子。
    virtual void OnStop();

    // 关闭钩子。
    virtual void OnShutdown();

    ComponentManager componentManager_;

private:
    // 信号处理入口。
    static void HandleSignal(int signo);

    std::atomic<bool> running_;
};

} // namespace sc
