#include "Component/Component.h"

#include <string>

namespace sc {

/// @brief 创建组件。
///
/// 初始引用计数为 1。
Component::Component() : refCount_(1)
{
}

/// @brief 销毁组件。
Component::~Component()
{
}

/// @brief 增加引用计数。
///
/// @return 增加后的引用计数。
unsigned int Component::AddRef()
{
    return refCount_.fetch_add(1) + 1;
}

/// @brief 减少引用计数。
///
/// 引用计数归零时销毁组件。
///
/// @return 减少后的引用计数。
unsigned int Component::Release()
{
    unsigned int count = refCount_.fetch_sub(1) - 1;
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
bool Component::QueryInterface(const InterfaceId& iid, void** ppv)
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
bool Component::QueryInterfaceImpl(const InterfaceId& iid, void** ppv)
{
    (void)iid;
    (void)ppv;
    return false;
}

} // namespace sc
