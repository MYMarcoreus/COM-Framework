#include "Module/Module.h"

#include <string>

namespace sc {

/// @brief 创建模块。
///
/// @param name 模块名称（进程内唯一，用于管理与日志）。
Module::Module(const char* name) : name_(name != nullptr ? name : "")
{
}

/// @brief 销毁模块。
Module::~Module()
{
}

/// @brief 获取模块名称。
const char* Module::GetName() const
{
    return name_.c_str();
}

/// @brief 默认初始化实现，子类按需重写。
///
/// @return true。
bool Module::Initialize()
{
    return true;
}

/// @brief 默认启动实现，子类按需重写。
///
/// @return true。
bool Module::Start()
{
    return true;
}

/// @brief 默认停止实现，子类按需重写。
void Module::Stop()
{
}

/// @brief 默认关闭实现，子类按需重写。
void Module::Shutdown()
{
}

/// @brief 接口查询实现。
///
/// 暴露 IModule 接口，其余接口交给 Component 继续分发。
///
/// @note 接口标识使用字符串内容比较（跨翻译单元地址不可靠，与现有组件实现一致）。
bool Module::QueryInterfaceImpl(const InterfaceId& iid, void** ppv)
{
    if (std::string(iid) == std::string(IID_IModule()))
    {
        *ppv = static_cast<IModule*>(this);
        return true;
    }
    return Component::QueryInterfaceImpl(iid, ppv);
}

} // namespace sc
