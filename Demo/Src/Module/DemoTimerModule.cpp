#include "Module/DemoTimerModule.h"

#include "Log/Logger.h"

namespace demo {

/// @brief 创建定时器模块。
///
/// @param intervalMs 周期日志间隔（毫秒），小于 100 时按 100 处理。
DemoTimerModule::DemoTimerModule(std::int64_t intervalMs)
    : sc::Module("timer"), intervalMs_(intervalMs), timerId_(common::kInvalidTimerId)
{
    if (intervalMs_ < 100)
    {
        intervalMs_ = 100;
    }
}

/// @brief 销毁定时器模块。
DemoTimerModule::~DemoTimerModule()
{
    Stop();
}

/// @brief 启动周期性定时器。
///
/// 每个周期输出一次运行状态日志，验证 Timer 模块与模块生命周期联动。
///
/// @return true 启动成功；false 启动失败。
bool DemoTimerModule::Start()
{
    if (!timer_.Start())
    {
        return false;
    }
    timerId_ = timer_.AddPeriodicTimer(intervalMs_,
        []()
        {
            common::Logger::Instance().Info("Demo 服务器运行中");
        });
    return true;
}

/// @brief 停止定时器。
void DemoTimerModule::Stop()
{
    timer_.Stop();
    timerId_ = common::kInvalidTimerId;
}

/// @brief 停止定时器并释放资源。
void DemoTimerModule::Shutdown()
{
    Stop();
}

} // namespace demo
