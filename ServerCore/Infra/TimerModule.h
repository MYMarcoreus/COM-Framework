#pragma once

#include <cstdint>

#include "Infra/ITimer.h"
#include "Module/InterfaceMap.h"
#include "Module/Module.h"
#include "Timer/TimerManager.h"

namespace sc {

/// @brief 定时器模块。
///
/// 内部持有 common::timer::CTimerManager 实例。
class CTimerModule : public CModule, public ITimer
{
public:
    CTimerModule();

    virtual ~CTimerModule();

    bool Initialize(const CResolveContext& ctx) override;
    bool Start() override;
    common::timer::TimerId AddTimer(std::int64_t delayMs,
                                    const common::timer::TimerCallback& callback) override;
    common::timer::TimerId AddPeriodicTimer(std::int64_t intervalMs,
                                            const common::timer::TimerCallback& callback) override;
    bool Cancel(common::timer::TimerId id) override;
    void Stop() override;
    void Shutdown() override;

    SC_DECLARE_INTERFACE_MAP();

private:
    common::timer::CTimerManager m_timer;
};

} // namespace sc
