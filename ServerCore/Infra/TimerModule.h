#pragma once

#include <cstdint>

#include "Infra/ITimer.h"
#include "Module/InterfaceMap.h"
#include "Module/Module.h"
#include "Timer/TimerManager.h"

namespace sc {

/// @brief 定时器模块。
///
/// 内部持有 common::CTimerManager 实例。
class CTimerModule : public CModule, public ITimer
{
public:
    CTimerModule();

    virtual ~CTimerModule();

    bool Initialize(const CResolveContext& ctx) override;
    bool Start() override;
    common::TimerId AddTimer(std::int64_t delayMs,
                             const common::TimerCallback& callback) override;
    common::TimerId AddPeriodicTimer(std::int64_t intervalMs,
                                     const common::TimerCallback& callback) override;
    bool Cancel(common::TimerId id) override;
    void Stop() override;
    void Shutdown() override;

    SC_DECLARE_INTERFACE_MAP();

private:
    common::CTimerManager m_timer;
};

} // namespace sc
