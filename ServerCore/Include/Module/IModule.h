#pragma once

#include <string>

#include "Component/IUnknown.h"

namespace sc {

/// @brief 模块生命周期状态。
///
/// 状态由 CModuleManager 统一维护并驱动转换：
/// kCreated → kInitialized → kStarted → kStopped → kShutdown。
enum class ModuleState
{
    kCreated,     // 已创建，尚未初始化
    kInitialized, // 已初始化，尚未启动
    kStarted,     // 已启动
    kStopped,     // 已停止，尚未关闭
    kShutdown     // 已关闭
};

/// @brief 获取 IModule 接口标识。
inline const InterfaceId& IID_IModule()
{
    static const InterfaceId iid = "sc::IModule";
    return iid;
}

/// @brief 模块接口（COM 风格：继承 IUnknown）。
///
/// 模块是服务器中可独立启停的功能单元，具有明确生命周期。
/// 与组件模型一致：可查询接口、引用计数、生命周期由 CModuleManager 统一管理。
class IModule : public virtual IUnknown
{
public:
    virtual ~IModule() {}

    // 模块名称（进程内唯一标识，用于管理与日志）。
    virtual const char* GetName() const = 0;

    // 初始化模块（创建资源、加载配置）。
    virtual bool Initialize() = 0;

    // 启动模块（开始工作）。
    virtual bool Start() = 0;

    // 停止模块（停止工作，资源保留）。
    virtual void Stop() = 0;

    // 关闭模块（释放资源）。
    virtual void Shutdown() = 0;

    // 状态报告：返回模块当前状态描述（如 "network:port=9000 conns=2"）。
    virtual std::string GetStatus() const = 0;
};

} // namespace sc
