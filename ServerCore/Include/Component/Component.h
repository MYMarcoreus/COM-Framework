#pragma once

#include <atomic>

#include "Component/IUnknown.h"

namespace sc {

/// @brief 组件基类。
///
/// 提供原子引用计数与接口查询骨架，具体组件继承本类并实现自身接口。
/// 创建时引用计数为 1，归零时自动销毁。
///
/// @note 通过虚继承 IUnknown，支持多接口菱形继承。
class CComponent : public virtual IUnknown
{
public:
    CComponent();

    virtual ~CComponent();

    // 增加引用计数。
    unsigned int AddRef() override;

    // 减少引用计数，归零时销毁组件。
    unsigned int Release() override;

    // 查询接口。
    bool QueryInterface(const InterfaceId& iid, void** ppv) override;

    // 生命周期：初始化（默认空实现，子类按需重写）。
    virtual bool Initialize();

    // 生命周期：启动（默认空实现，子类按需重写）。
    virtual bool Start();

    // 生命周期：停止（默认空实现，子类按需重写）。
    virtual void Stop();

    // 生命周期：关闭（默认空实现，子类按需重写）。
    virtual void Shutdown();

protected:
    // 子类重写以返回自身实现的接口。
    virtual bool QueryInterfaceImpl(const InterfaceId& iid, void** ppv);

private:
    std::atomic<unsigned int> m_nRefCount;
};

} // namespace sc
