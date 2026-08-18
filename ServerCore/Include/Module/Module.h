#pragma once

#include <string>

#include "Component/Component.h"
#include "Module/IModule.h"

namespace sc {

/// @brief 模块基类（引用计数 + 默认生命周期空实现）。
///
/// 继承 Component 获得原子引用计数与接口查询骨架，实现 IModule 接口。
/// 子类只需提供模块名称，并按需重写生命周期方法。
class Module : public Component, public IModule
{
public:
    explicit Module(const char* name);

    virtual ~Module();

    // 模块名称。
    const char* GetName() const override;

    // 默认空实现，子类按需重写。
    bool Initialize() override;
    bool Start() override;
    void Stop() override;
    void Shutdown() override;

protected:
    // 接口查询实现：暴露 IModule 接口。
    bool QueryInterfaceImpl(const InterfaceId& iid, void** ppv) override;

private:
    std::string name_;
};

} // namespace sc
