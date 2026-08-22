#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "Module/RefObject.h"
#include "Module/IModule.h"

namespace sc {

/// @brief 模块基类（统一实现骨架）。
///
/// 继承 CRefObject 获得引用计数 / 强引用 / 弱引用能力；
/// 提供接口查询骨架、默认生命周期空实现与状态报告。
/// 具体模块继承本类并按需重写生命周期方法；通过实现额外接口暴露业务能力。
/// 创建时引用计数为 1，归零时自动销毁。
// 前向声明（Initialize 通过引用接收，无需完整定义）。
class CResolveContext;

class CModule : public CRefObject, public IModule
{
public:
    explicit CModule(const char* strName = "");

    virtual ~CModule();

    // 模块名称（由构造函数指定，框架管理，子类不应重写）。
    const char* GetName() const final;

    // 当前生命周期状态（由 CModuleManager 驱动维护，供日志/健康检查查询；
    // 框架管理，子类不应重写）。
    ModuleState GetState() const final;

    // 本模块依赖的接口标识列表（由 AddDependency 声明，供生命周期拓扑排序；
    // 框架管理，子类不应重写）。
    const std::vector<InterfaceId>& GetDependencies() const final;

    // 生命周期方法（纯虚）：具体模块必须实现生命周期行为，框架不提供默认实现；
    // 依赖通过注入的初始化上下文在 Initialize 中解析。
    bool Initialize(const CResolveContext& ctx) override = 0;
    bool Start() override = 0;
    void Stop() override = 0;
    void Shutdown() override = 0;

    // 默认状态报告：返回模块名称，子类按需重写。
    std::string GetStatus() const override;

    // 返回指向自身的强引用（自持引用，IModule 视图）。
    // 注册回调 / 投递异步任务时调用，把自持引用一并传入回调，
    // 保证回调执行期间模块存活；回调对象销毁时引用自动释放。
    // 需要其它接口视图时使用基类模板：Self<INetwork>() / WeakSelf<INetwork>()。
    using CRefObject::Self;
    using CRefObject::WeakSelf;
    ScopedInterfacePtr<IModule> Self();

    // 返回指向自身的弱引用（不延长模块生命周期，IModule 视图）。
    // 适用于异步回调 / 结果通知等"模块已关闭则丢弃结果"的场景：
    // 模块 Shutdown 并析构后弱引用自动失效（Expired），
    // Lock() 升级为强引用时若模块已销毁则返回空，不会阻止模块销毁。
    CWeakPtr<IModule> WeakSelf();

protected:
    // 子类重写以返回自身实现的接口。
    void* QueryInterfaceImpl(const InterfaceId& iid) override;

    // 声明本模块依赖的接口标识（构造函数中调用），供生命周期拓扑排序。
    void AddDependency(const InterfaceId& iid);

private:
    // 仅供 CModuleManager 在生命周期编排时调用，外部不可直接修改。
    void SetState(ModuleState state);

    friend class CModuleManager;

    std::atomic<ModuleState> m_state;
    std::string m_strName;
    std::vector<InterfaceId> m_vecDependencies;
};

} // namespace sc
