#pragma once

#include <cstdint>

#include "Module/ScopedInterfacePtr.h"
#include "Infra/ITimer.h"
#include "Module/Module.h"

namespace demo {

/// @brief 定时器模块。
///
/// 启动后周期性输出运行状态，通过 ITimer 接口驱动（不直接持有 CTimerManager）。
/// 模块名 "timer"。
class CDemoTimerModule : public sc::CModule
{
public:
    explicit CDemoTimerModule(std::int64_t intervalMs);

    virtual ~CDemoTimerModule();

    // 从初始化上下文解析 ITimer 接口。
    bool Initialize(const sc::CResolveContext& ctx) override;

    // 启动周期性定时器。
    bool Start() override;

    // 取消定时器（不停止共享的 ITimer）。
    void Stop() override;

    // 停止定时器并释放接口引用。
    void Shutdown() override;

private:
    std::int64_t m_nIntervalMs;
    sc::ScopedInterfacePtr<sc::ITimer> m_pTimer;
    common::timer::TimerId m_tTimerId;
};

} // namespace demo
