#pragma once

#include "Module/ModuleManager.h"
#include "Module/InterfaceId.h"

namespace sc {

/// @brief 依赖解析上下文（依赖注入）。
///
/// 模块在 Initialize(const CResolveContext&) 中通过它解析依赖接口。
/// 相比直接持有 CModuleManager&：
///  - 依赖变为显式注入参数，模块不再持有装配器全局引用，便于单测
///    （测试可构造 CModuleManager + 注册 mock 接口模块后传入）；
///  - Resolve<T>() 自动绑定"类型 ↔ 接口标识"，少一个"iid 与类型不匹配"的错误源；
///  - 自定义接口（无 InterfaceIdOf 特化）仍可用显式 iid 版本 Resolve<T>(iid)。
class CResolveContext
{
public:
    explicit CResolveContext(CModuleManager& manager) : m_manager(manager) {}

    // 按类型自动绑定接口标识解析模块（借用指针，不增加引用计数）。
    template <typename T>
    T* Resolve() const
    {
        return m_manager.Resolve<T>(InterfaceIdOf<T>::Get());
    }

    // 显式指定接口标识解析（自定义接口时使用；借用指针，不增加引用计数）。
    template <typename T>
    T* Resolve(const InterfaceId& iid) const
    {
        return m_manager.Resolve<T>(iid);
    }

private:
    CModuleManager& m_manager;
};

} // namespace sc
