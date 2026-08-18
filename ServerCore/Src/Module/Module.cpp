#include "Module/Module.h"

#include <string>

namespace sc {

/// @brief 创建模块。
///
/// @param strName 模块名称（进程内唯一，用于管理与日志）。
CModule::CModule(const char* strName) : m_strName(strName != nullptr ? strName : "")
{
}

/// @brief 销毁模块。
CModule::~CModule()
{
}

/// @brief 获取模块名称。
const char* CModule::GetName() const
{
    return m_strName.c_str();
}

/// @brief 默认初始化实现，子类按需重写。
///
/// @return true。
bool CModule::Initialize()
{
    return true;
}

/// @brief 默认启动实现，子类按需重写。
///
/// @return true。
bool CModule::Start()
{
    return true;
}

/// @brief 默认停止实现，子类按需重写。
void CModule::Stop()
{
}

/// @brief 默认关闭实现，子类按需重写。
void CModule::Shutdown()
{
}

/// @brief 接口查询实现。
///
/// 暴露 IModule 接口，其余接口交给 CComponent 继续分发。
///
/// @note 接口标识使用字符串内容比较（跨翻译单元地址不可靠，与现有组件实现一致）。
bool CModule::QueryInterfaceImpl(const InterfaceId& iid, void** ppv)
{
    if (std::string(iid) == std::string(IID_IModule()))
    {
        *ppv = static_cast<IModule*>(this);
        return true;
    }
    return CComponent::QueryInterfaceImpl(iid, ppv);
}

} // namespace sc
