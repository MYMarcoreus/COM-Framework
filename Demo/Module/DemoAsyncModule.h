#pragma once

#include <cstdint>
#include <memory>

#include "Async/AsyncExecutor.h"
#include "Module/ScopedInterfacePtr.h"
#include "Infra/ITimer.h"
#include "Module/Module.h"

namespace demo {

/// @brief 异步框架演示模块。
///
/// 演示 common::async::CAsyncExecutor / CTask 的异步能力：
///  - 链式调用：Submit → Then → Get（多阶段流水线）；
///  - 多回调 fan-out：同一任务多个 OnSuccess 消费者；
///  - 任务异常 → 无值终止（不抛异常）；
///  - 阻塞获取：Get()。
///
/// 周期性（ITimer + 弱引用守卫）触发一次完整演示。
/// 模块名 "async-demo"。
class CDemoAsyncModule : public sc::CModule
{
public:
    explicit CDemoAsyncModule(std::int64_t intervalMs);

    virtual ~CDemoAsyncModule();

    // 解析 ITimer 接口。
    bool Initialize(const sc::CResolveContext& ctx) override;

    // 启动自建执行器并注册周期演示定时器。
    bool Start() override;

    // 取消定时器并停止执行器（等待任务完成）。
    void Stop() override;

    // 停止并释放引用。
    void Shutdown() override;

private:
    // 执行一次完整的异步能力演示。
    void RunDemo();

    std::int64_t m_nIntervalMs;
    sc::ScopedInterfacePtr<sc::ITimer> m_pTimer;
    std::unique_ptr<common::async::CAsyncExecutor> m_pExecutor; // 异步执行器（Option 风格）
    common::timer::TimerId m_tTimerId;                                 // 周期演示定时器 id
};

} // namespace demo
