#include "Module/RefObject.h"

#include <mutex>

namespace sc {

/// @brief 创建引用对象。
///
/// 初始引用计数为 1，并创建堆上存活状态（供弱引用判断）。
CRefObject::CRefObject()
    : m_pLifetime(std::make_shared<detail::CLifetime>()),
      m_nRefCount(1)
{
}

/// @brief 销毁引用对象。
///
/// 标记存活状态为死亡（兜底：未走 Release 归零路径的裸 delete 也能正确失效弱引用）。
CRefObject::~CRefObject()
{
    std::shared_ptr<detail::CLifetime> spLifetime = m_pLifetime;
    if (spLifetime)
    {
        std::lock_guard<std::mutex> guard(spLifetime->m_mutex);
        spLifetime->MarkDead();
    }
}

/// @brief 增加引用计数。
///
/// @return 增加后的引用计数。
unsigned int CRefObject::AddRef()
{
    return m_nRefCount.fetch_add(1) + 1;
}

/// @brief 减少引用计数。
///
/// 引用计数归零时销毁对象。
/// 归零瞬间在存活锁内标记死亡（与 CWeakPtr::Lock 的"检查 + AddRef"互斥），
/// 保证弱引用不会在对象进入析构后错误升级。
///
/// @return 减少后的引用计数。
unsigned int CRefObject::Release()
{
    unsigned int nCount = m_nRefCount.fetch_sub(1) - 1;
    if (nCount == 0)
    {
        std::shared_ptr<detail::CLifetime> spLifetime = m_pLifetime;
        if (spLifetime)
        {
            std::lock_guard<std::mutex> guard(spLifetime->m_mutex);
            spLifetime->MarkDead();
        }
        delete this;
    }
    return nCount;
}

/// @brief 查询接口。
///
/// 优先匹配 IUnknown，其余接口交给子类的 QueryInterfaceImpl。
///
/// @param iid 接口标识。
///
/// @return 借用的接口指针；未找到返回 nullptr。
void* CRefObject::QueryInterface(const InterfaceId& iid)
{
    if (!iid.IsValid())
    {
        return nullptr;
    }
    if (iid == IID_IUnknown())
    {
        return static_cast<IUnknown*>(this);
    }
    return QueryInterfaceImpl(iid);
}

/// @brief 接口查询默认实现。
///
/// 基础对象只暴露 IUnknown；子类重写以暴露自身实现的接口。
///
/// @return nullptr。
void* CRefObject::QueryInterfaceImpl(const InterfaceId& /*iid*/)
{
    return nullptr;
}

} // namespace sc
