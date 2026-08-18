#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include "Component/ComponentManager.h"
#include "Module/ModuleManager.h"

namespace sc {

/// @brief 服务器应用程序基础类。
///
/// 提供统一的初始化、启动、运行、停止和关闭生命周期。
/// 通过 ComponentManager 组合基础组件，通过 ModuleManager 统一管理模块生命周期，
/// 不硬编码具体实现。
///
/// @note 派生类通过重写虚函数扩展服务器生命周期与组件/模块装配。
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

    // 获取模块管理器。
    ModuleManager& GetModuleManager();

    // 设置配置文件路径（派生类在加载配置时读取）。
    void SetConfigPath(const std::string& path);

    // 配置文件路径。
    const std::string& ConfigPath() const;

    // 是否正在运行。
    bool IsRunning() const;

    // 已运行秒数（Run 启动后计时，供状态查询）。
    uint64_t UptimeSeconds() const;

protected:
    // 注册应用程序需要的基础组件。
    virtual bool RegisterComponents();

    // 注册应用程序需要的模块。
    virtual bool RegisterModules();

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
    ModuleManager moduleManager_;

private:
    // 信号处理入口。
    static void HandleSignal(int signo);

    std::string configPath_;
    std::chrono::steady_clock::time_point startTime_;
    std::atomic<bool> running_;
};

} // namespace sc
