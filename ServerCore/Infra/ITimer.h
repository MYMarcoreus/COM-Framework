#pragma once

#include <cstdint>

#include "Module/IUnknown.h"
#include "Module/InterfaceDecl.h"
#include "Timer/TimerManager.h"

namespace sc {

/// @brief 定时器接口（模块化适配 common::CTimerManager）。
///
/// 使模块通过模块管理器按接口使用定时能力。
SC_INTERFACE(ITimer, "sc::ITimer", "4810a33f-4a1c-490e-b19f-a2d1f3d4229d")
{
public:
    virtual ~ITimer() {}

    // 启动定时器线程。
    virtual bool Start() = 0;

    // 添加一次性定时器。
    virtual common::TimerId AddTimer(std::int64_t delayMs,
                                     const common::TimerCallback& callback) = 0;

    // 添加周期性定时器。
    virtual common::TimerId AddPeriodicTimer(std::int64_t intervalMs,
                                             const common::TimerCallback& callback) = 0;

    // 取消定时器。
    virtual bool Cancel(common::TimerId id) = 0;

    // 停止定时器线程。
    virtual void Stop() = 0;
};

} // namespace sc
