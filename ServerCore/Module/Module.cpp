#include "Module/Module.h"

#include <string>

namespace sc {

/// @brief 创建模块。
///
/// 引用计数与存活状态由 CRefObject 基类管理，初始引用计数为 1。
///
/// @param strName 模块名称（进程内唯一，用于管理与日志；可为空）。
CModule::CModule(const char* strName)
    : m_state(ModuleState::kCreated),
      m_strName(strName != nullptr ? strName : "")
{
}

/// @brief 销毁模块。
///
/// 引用计数与存活状态由 CRefObject 基类管理（析构时自动标记死亡）。
CModule::~CModule()
{
}

/// @brief 获取模块名称。
const char* CModule::GetName() const
{
    return m_strName.c_str();
}

/// @brief 获取本模块依赖的接口标识列表。
///
/// @return 依赖列表（由派生类构造函数通过 AddDependency 声明）。
const std::vector<InterfaceId>& CModule::GetDependencies() const
{
    return m_vecDependencies;
}

/// @brief 声明本模块依赖的接口标识。
///
/// 在派生类构造函数中调用；CModuleManager 生命周期编排时按依赖拓扑排序，
/// 保证被依赖的接口模块先于本模块初始化 / 启动。
///
/// @param iid 接口标识（无效标识忽略）。
void CModule::AddDependency(const InterfaceId& iid)
{
    if (iid.IsValid())
    {
        m_vecDependencies.push_back(iid);
    }
}

/// @brief 当前生命周期状态。
///
/// @note 由 CModuleManager 在生命周期编排时驱动更新。
ModuleState CModule::GetState() const
{
    return m_state.load();
}

/// @brief 设置生命周期状态（仅供 CModuleManager 调用）。
void CModule::SetState(ModuleState state)
{
    m_state.store(state);
}

/// @brief 默认状态报告。
///
/// @note 默认返回模块名称，子类按需重写以提供更有意义的状态。
std::string CModule::GetStatus() const
{
    return m_strName.empty() ? "module" : m_strName;
}

/// @brief 返回指向自身的强引用（自持引用）。
///
/// 在注册回调 / 投递异步任务时调用，将自持引用一并传入回调：
/// 回调执行期间模块必然存活（即使正在关闭）；回调对象销毁时引用自动释放。
/// 模块 Shutdown 时必须取消 / 解除自己注册的所有回调，引用才能归零并析构。
///
/// @return 指向 this 的强引用（引用计数 +1）。
sc::ScopedInterfacePtr<IModule> CModule::Self()
{
    return sc::ScopedInterfacePtr<IModule>(static_cast<IModule*>(this));
}

/// @brief 返回指向自身的弱引用（不延长生命周期）。
///
/// 与 Self() 对应：弱引用不持有模块，模块 Shutdown 析构后自动失效。
/// 在"模块已关闭则丢弃结果"的异步回调 / 结果通知中使用。
///
/// @return 指向 this 的弱引用。
sc::CWeakPtr<sc::IModule> CModule::WeakSelf()
{
    return sc::CWeakPtr<sc::IModule>(static_cast<IModule*>(this), m_pLifetime);
}

/// @brief 接口查询实现。
///
/// 暴露 IModule 接口，其余接口交给子类继续分发。
///
/// @note 接口标识使用字符串内容比较（跨翻译单元地址不可靠，与现有模块实现一致）。
void* CModule::QueryInterfaceImpl(const InterfaceId& iid)
{
    if (iid == IID_IModule())
    {
        return static_cast<IModule*>(this);
    }
    return nullptr;
}

} // namespace sc
