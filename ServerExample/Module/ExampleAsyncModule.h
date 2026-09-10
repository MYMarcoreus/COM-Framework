#pragma once

#include <cstdint>
#include <memory>

#include "Async/AsyncExecutor.h"
#include "Infra/ITimer.h"
#include "Module/Module.h"
#include "Module/ScopedInterfacePtr.h"

namespace serverexample {

/// @brief 异步链演示模块。
///
/// 演示 common::async 异步链特化版的用法（周期触发一次完整演示）：
///  - 链 + 共享上下文：层与层之间只传成功 / 失败，数据走 shared_ptr 上下文；
///  - 失败即停：某层失败后后续 Then 层不再执行，失败码透传；
///  - ThenAlways：失败也执行的层（回滚 / 补偿 / 清理），可观察上一层失败；
///  - 层内异常 → 本层失败（kStepException），不向调用方抛出；
///  - 协程：用顺序代码 await 多条子链（失败则协程终止）。
///
/// 模块名 "async-example"。
class CExampleAsyncModule : public sc::CModule
{
   public:
    explicit CExampleAsyncModule(std::int64_t intervalMs);

    virtual ~CExampleAsyncModule();

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
    void RunExample();

    std::int64_t m_nIntervalMs;
    sc::ScopedInterfacePtr<sc::ITimer> m_pTimer;
    std::unique_ptr<common::async::CAsyncExecutor> m_pExecutor;  // 异步执行器（链 / 协程调度）
    common::timer::TimerId m_tTimerId;                           // 周期演示定时器 id
};

}  // namespace serverexample
