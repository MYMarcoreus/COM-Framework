#include "Component/Component.h"

#include <string>

namespace sc {

/// @brief 创建组件。
///
/// 初始引用计数为 1。
CComponent::CComponent() : m_nRefCount(1)
{
}

/// @brief 销毁组件。
CComponent::~CComponent()
{
}

/// @brief 增加引用计数。
///
/// @return 增加后的引用计数。
unsigned int CComponent::AddRef()
{
    return m_nRefCount.fetch_add(1) + 1;
}

/// @brief 减少引用计数。
///
/// 引用计数归零时销毁组件。
///
/// @return 减少后的引用计数。
unsigned int CComponent::Release()
{
    unsigned int count = m_nRefCount.fetch_sub(1) - 1;
    if (count == 0)
    {
        delete this;
    }
    return count;
}

/// @brief 查询接口。
///
/// 优先匹配 IUnknown，其余接口交给子类的 QueryInterfaceImpl。
///
/// @param iid 接口标识。
/// @param ppv 输出接口指针。
///
/// @return true 查询成功；false 未找到接口。
bool CComponent::QueryInterface(const InterfaceId& iid, void** ppv)
{
    if (ppv == nullptr)
    {
        return false;
    }
    *ppv = nullptr;
    if (iid == nullptr)
    {
        return false;
    }
    if (std::string(iid) == std::string(IID_IUnknown()))
    {
        *ppv = static_cast<IUnknown*>(this);
        return true;
    }
    return QueryInterfaceImpl(iid, ppv);
}

/// @brief 子类接口查询实现。
///
/// 基类不实现任何业务接口，子类通过重写返回自身接口。
///
/// @return 默认返回 false。
bool CComponent::QueryInterfaceImpl(const InterfaceId& iid, void** ppv)
{
    (void)iid;
    (void)ppv;
    return false;
}

/// @brief 默认初始化实现，子类按需重写。
///
/// @return true。
bool CComponent::Initialize()
{
    return true;
}

/// @brief 默认启动实现，子类按需重写。
///
/// @return true。
bool CComponent::Start()
{
    return true;
}

/// @brief 默认停止实现，子类按需重写。
void CComponent::Stop()
{
}

/// @brief 默认关闭实现，子类按需重写。
void CComponent::Shutdown()
{
}

/// @brief 返回组件状态描述。
///
/// @note 默认返回通用描述，子类按需重写以提供更有意义的状态。
std::string CComponent::GetStatus() const
{
    return "component";
}

} // namespace sc
