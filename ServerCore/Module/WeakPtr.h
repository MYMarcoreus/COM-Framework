#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <type_traits>

#include "Module/ScopedInterfacePtr.h"

namespace sc {

namespace detail {

/// @brief 对象存活状态（堆上共享对象，供弱引用安全判断）。
///
/// CRefObject 构造时创建（存活）；最后一个引用释放归零 / 析构时置为死亡。
/// CWeakPtr 持有其 weak_ptr：Lock() 先升级为 shared_ptr（保证本对象不被释放），
/// 再在 m_mutex 保护下完成"存活检查 + AddRef"，
/// 与 CRefObject::Release 归零时的"MarkDead + 析构"互斥，
/// 从而保证绝不访问已销毁的对象内存。
class CLifetime
{
public:
    CLifetime() : m_bAlive(true) {}

    // 是否存活（调用方须持有 m_mutex）。
    bool IsAlive() const
    {
        return m_bAlive.load(std::memory_order_acquire);
    }

    // 置为死亡（调用方须持有 m_mutex）。
    void MarkDead()
    {
        m_bAlive.store(false, std::memory_order_release);
    }

    // 保护"IsAlive 检查 + AddRef"与"MarkDead + 析构"互斥。
    std::mutex m_mutex;

private:
    std::atomic<bool> m_bAlive;
};

} // namespace detail

/// @brief 弱引用（不延长对象生命周期）。
///
/// 与 ScopedInterfacePtr（强引用）对应：弱引用不持有对象，
/// 对象析构后自动失效（Expired），不会阻止对象销毁。
/// 适用于异步回调 / 结果通知等"对象已关闭则丢弃结果"的场景。
///
/// 用法：
/// @code
///   sc::CWeakPtr<sc::IModule> spWeak = WeakSelf();
///   m_pExecutor->Post([spWeak]()
///   {
///       sc::ScopedInterfacePtr<sc::IModule> spStrong = spWeak.Lock();
///       if (!spStrong)
///       {
///           return;   // 对象已销毁，丢弃结果
///       }
///       /* 安全使用 spStrong */
///   });
/// @endcode
///
/// @tparam T 接口类型，必须是 IUnknown 派生类型。
template <typename T>
class CWeakPtr
{
    static_assert(std::is_base_of<IUnknown, T>::value,
                  "CWeakPtr<T> 要求 T 必须是 IUnknown 派生接口");

public:
    // 创建空弱引用。
    CWeakPtr() : m_ptr(nullptr) {}

    // 是否已失效（模块已销毁或从未绑定）。
    bool Expired() const
    {
        return m_ptr == nullptr || m_pLifetime.expired();
    }

    // 升级为强引用：模块存活返回有效强引用；已销毁返回空。
    ScopedInterfacePtr<T> Lock() const
    {
        if (m_ptr == nullptr)
        {
            return ScopedInterfacePtr<T>();
        }
        std::shared_ptr<detail::CLifetime> spLifetime = m_pLifetime.lock();
        if (!spLifetime)
        {
            return ScopedInterfacePtr<T>();
        }
        // 与"归零 MarkDead + 析构"互斥：存活检查与 AddRef 是一个原子临界区。
        std::lock_guard<std::mutex> guard(spLifetime->m_mutex);
        if (!spLifetime->IsAlive())
        {
            return ScopedInterfacePtr<T>();
        }
        // 模块存活（析构尚未开始）：AddRef 后模块不会被销毁。
        m_ptr->AddRef();
        return ScopedInterfacePtr<T>::Adopt(m_ptr);
    }

    // 释放弱引用。
    void Reset()
    {
        m_ptr = nullptr;
        m_pLifetime.reset();
    }

    // 返回借用的裸指针（不保证存活，仅供同步上下文 / 调试）。
    T* UnsafeGet() const { return m_ptr; }

    // 是否仍有效（未被销毁）。
    explicit operator bool() const { return !Expired(); }

private:
    friend class CModule;
    friend class CRefObject;

    CWeakPtr(T* ptr, const std::shared_ptr<detail::CLifetime>& spLifetime)
        : m_ptr(ptr), m_pLifetime(spLifetime)
    {
    }

    T* m_ptr;
    std::weak_ptr<detail::CLifetime> m_pLifetime;
};

} // namespace sc
