#pragma once

#include <atomic>
#include <string>

#include "Module/IModule.h"

namespace sc {

/// @brief 模块基类（统一实现骨架）。
///
/// 提供原子引用计数、接口查询骨架、默认生命周期空实现与状态报告。
/// 具体模块继承本类并按需重写生命周期方法；通过实现额外接口暴露业务能力。
/// 创建时引用计数为 1，归零时自动销毁。
class CModule : public IModule
{
public:
    explicit CModule(const char* strName = "");

    virtual ~CModule();

    // 增加引用计数。
    unsigned int AddRef() override;

    // 减少引用计数，归零时销毁模块。
    unsigned int Release() override;

    // 查询接口。
    bool QueryInterface(const InterfaceId& iid, void** ppv) override;

    // 模块名称。
    const char* GetName() const override;

    // 默认空实现，子类按需重写。
    bool Initialize() override;
    bool Start() override;
    void Stop() override;
    void Shutdown() override;

    // 默认状态报告：返回模块名称，子类按需重写。
    std::string GetStatus() const override;

protected:
    // 子类重写以返回自身实现的接口。
    virtual bool QueryInterfaceImpl(const InterfaceId& iid, void** ppv);

private:
    std::atomic<unsigned int> m_nRefCount;
    std::string m_strName;
};

} // namespace sc
