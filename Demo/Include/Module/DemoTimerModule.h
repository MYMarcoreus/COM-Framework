#pragma once

#include <cstdint>

#include "Module/Module.h"
#include "Timer/TimerManager.h"

namespace demo {

/// @brief 定时器模块。
///
/// 启动后周期性输出运行状态，演示 Timer 模块与模块生命周期。
/// 模块名 "timer"。
class DemoTimerModule : public sc::Module
{
public:
    explicit DemoTimerModule(std::int64_t intervalMs);

    virtual ~DemoTimerModule();

    // 启动周期性定时器。
    bool Start() override;

    // 停止定时器。
    void Stop() override;

    // 停止定时器并释放资源。
    void Shutdown() override;

private:
    std::int64_t intervalMs_;
    common::TimerManager timer_;
    common::TimerId timerId_;
};

} // namespace demo
