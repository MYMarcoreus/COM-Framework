#pragma once

#include <cstdint>

#include "Timer/TimerManager.h"
#include "Module/WeakPtr.h"

namespace sc {

/// @brief 注册带弱引用生命周期守卫的周期定时器。
///
/// 定时器内部持有弱引用，每次到期 Lock()：仅当目标对象存活时调用回调
/// （回调参数为已升级的强引用，类型 T 由弱引用决定，可为接口或具体类型，
/// 无需转换）；对象销毁后回调自动跳过，不延长对象生命周期。
///
/// 用法：
/// @code
///   m_tTimerId = sc::AddGuardedPeriodicTimer(m_pTimer.Get(), m_nIntervalMs,
///       WeakSelf<CDemoXxx>(),              // 弱引用目标类型（决定回调参数类型）
///       [](const sc::ScopedInterfacePtr<CDemoXxx>& sp)
///       {
///           sp->DoSomething();              // 具体类型直接调用，无需转换
///       });
/// @endcode
///
/// @tparam TimerT 定时器类型（sc::ITimer 或 common::CTimerManager，须提供
///                AddPeriodicTimer(int64, const common::TimerCallback&)）
/// @tparam T      弱引用目标类型（IModule / 具体类型等，须为 IUnknown 派生）
/// @tparam F      回调类型（lambda / 函数对象，推导得到）
///
/// @param pTimer       定时器对象指针（非空）。
/// @param nIntervalMs  周期（毫秒）。
/// @param spWeak       目标对象的弱引用（注册时捕获，不延长生命周期）。
/// @param fnCallback   回调，参数为已升级的强引用（仅存活时调用）。
///
/// @return 定时器标识（取消时用）。
template <typename TimerT, typename T, typename F>
common::TimerId AddGuardedPeriodicTimer(
    TimerT* pTimer, std::int64_t nIntervalMs,
    const CWeakPtr<T>& spWeak, F fnCallback)
{
    return pTimer->AddPeriodicTimer(nIntervalMs,
        [spWeak, fnCallback]()
        {
            ScopedInterfacePtr<T> sp = spWeak.Lock();
            if (sp && fnCallback)
            {
                fnCallback(sp);
            }
        });
}

} // namespace sc
