#pragma once

#include <atomic>
#include <memory>
#include <type_traits>

#include "Module/IUnknown.h"
#include "Module/InterfaceId.h"
#include "Module/ScopedInterfacePtr.h"
#include "Module/WeakPtr.h"

namespace sc {

namespace detail {

/// @brief 自持引用模板参数约束（编译期校验 Self<T>() / WeakSelf<T>() 的 T）。
///
/// 约束：
///  - T 必须继承自 IUnknown（接口类型，或继承 CRefObject 的具体对象）；
///  - T 不能是指针 / 引用类型；
///  - T 不能带 const / volatile 限定。
template <typename T>
struct IsSelfable
{
    static const bool value =
        std::is_base_of<IUnknown, T>::value &&
        !std::is_pointer<T>::value &&
        !std::is_reference<T>::value &&
        !std::is_const<T>::value &&
        !std::is_volatile<T>::value;
};

} // namespace detail

/// @brief 可被强引用 / 弱引用的基础对象。
///
/// 提供原子引用计数（AddRef / Release）、接口查询骨架、
/// 自持强引用（Self）与弱引用（WeakSelf）。
/// 需要引用 / 弱引用能力的对象（模块、连接、业务上下文等）继承本类复用，
/// 无需自行实现引用计数与生命周期判断。
///
/// 线程安全：
///  - AddRef / Release 为无锁原子计数；归零销毁与弱引用 Lock 在同一把锁内互斥。
///  - WeakSelf().Lock() 保证绝不访问已销毁的对象。
class CRefObject : public virtual IUnknown
{
public:
    CRefObject();

    virtual ~CRefObject();

    // 增加引用计数。
    unsigned int AddRef() override;

    // 减少引用计数，归零时销毁对象。
    unsigned int Release() override;

    // 查询接口（优先 IUnknown，其余交给 QueryInterfaceImpl）。
    void* QueryInterface(const InterfaceId& iid) override;

    // 返回指向自身的强引用（自持引用）。
    // 无参数 Self() 等价于 Self<IUnknown>()；指定接口视图用 Self<IMyInterface>()，
    // T 必须是 IUnknown 派生接口（编译期约束见 detail::IsSelfable），且为 this 实际继承的接口。
    template <typename T = IUnknown>
    ScopedInterfacePtr<T> Self()
    {
        static_assert(detail::IsSelfable<T>::value,
                      "Self<T>(): T 必须是 IUnknown 派生接口，且不能是指针/引用/cv 限定类型");
        return ScopedInterfacePtr<T>(dynamic_cast<T*>(this));
    }

    // 返回指向自身的弱引用（不延长生命周期）。
    // 无参数 WeakSelf() 等价于 WeakSelf<IUnknown>()；指定接口视图用 WeakSelf<T>()。
    template <typename T = IUnknown>
    CWeakPtr<T> WeakSelf()
    {
        static_assert(detail::IsSelfable<T>::value,
                      "WeakSelf<T>(): T 必须是 IUnknown 派生接口，且不能是指针/引用/cv 限定类型");
        return CWeakPtr<T>(dynamic_cast<T*>(this), m_pLifetime);
    }

protected:
    // 子类重写以返回自身实现的接口。
    virtual void* QueryInterfaceImpl(const InterfaceId& iid);

    // 堆上共享的存活状态（供弱引用判断对象是否已销毁）。
    std::shared_ptr<detail::CLifetime> m_pLifetime;

private:
    std::atomic<unsigned int> m_nRefCount;
};

} // namespace sc
