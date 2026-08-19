#pragma once

#include <cstdint>

#include "Module/Module.h"
#include "Timer/TimerManager.h"

namespace sc {

/// @brief 获取 ITimer 接口标识。
inline const InterfaceId& IID_ITimer()
{
    static const InterfaceId iid("sc::ITimer", "4810a33f-4a1c-490e-b19f-a2d1f3d4229d");
    return iid;
}

/// @brief 定时器接口（模块化适配 common::CTimerManager）。
///
/// 使模块通过模块管理器按接口使用定时能力。
class ITimer : public virtual IUnknown
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

/// @brief 定时器模块。
///
/// 内部持有 common::CTimerManager 实例。
class CTimerModule : public CModule, public ITimer
{
public:
    CTimerModule();

    virtual ~CTimerModule();

    bool Start() override;
    common::TimerId AddTimer(std::int64_t delayMs,
                             const common::TimerCallback& callback) override;
    common::TimerId AddPeriodicTimer(std::int64_t intervalMs,
                                     const common::TimerCallback& callback) override;
    bool Cancel(common::TimerId id) override;
    void Stop() override;

protected:
    // 接口查询实现。
    bool QueryInterfaceImpl(const InterfaceId& iid, void** ppv) override;

private:
    common::CTimerManager m_timer;
};

} // namespace sc
