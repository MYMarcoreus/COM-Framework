#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

#include "Exec/CallbackStack.h"
#include "Exec/ModuleScheduler.h"

namespace sc {

/// @brief 业务流程：一次业务处理的载体。
///
///  - 贯穿全程的回调栈（跨模块、跨线程共享，线程安全）；
///  - 子任务计数：主体投递子任务后立即返回，待全部子任务排空后
///    回放回调栈（LIFO 逐个出栈触发）；
///  - SubmitTask 自动完成 BeginTask/EndTask，避免漏配导致流程不结束；
///  - 通过 enable_shared_from_this 在异步子任务中保活：
///    子任务必须按值捕获流程的 shared_ptr，禁止跨线程按引用持有流程。
///
/// 约束：
///  - 必须由 CGlobalDispatcher::Dispatch 以 shared_ptr 创建后使用；
///  - 子任务内可继续提交子任务（含 Complete() 之后、流程结束之前），
///    只要在流程回调栈回放（m_bFinished）之前提交即可；
///  - 回调栈回放期间/之后不得再向本流程提交子任务。
class CBusinessFlow : public std::enable_shared_from_this<CBusinessFlow>
{
public:
    CBusinessFlow();
    virtual ~CBusinessFlow();

    /// @brief 流程回调栈（业务处理中压栈，流程结束回放）。
    CCallbackStack& Callbacks() { return m_callbacks; }

    /// @brief 提交一个读/写子任务到模块调度器。
    ///
    /// 自动完成 BeginTask/EndTask；子任务内可继续调用其他模块调度器
    /// （嵌套子任务同样自动计数）。
    ///
    /// @param pScheduler 目标模块调度器（不得为空）。
    /// @param eKind 子任务类型（读/写）。
    /// @param fnTask 子任务逻辑（不应抛异常；框架捕获并记录日志）。
    /// @return true 已投递或已排队；false 调度器为空或线程池不可用。
    bool SubmitTask(CModuleScheduler* pScheduler, CModuleScheduler::ETaskKind eKind,
                    const std::function<void()>& fnTask);

    /// @brief 标记一个子任务开始（SubmitTask 内部使用）。
    void BeginTask();

    /// @brief 标记一个子任务结束（SubmitTask 包装内部使用）。
    void EndTask();

    /// @brief 流程主体结束：不再投递新子任务；全部子任务排空后回放回调栈。
    void Complete();

    /// @brief 是否已回放回调栈。
    bool IsFinished() const { return m_bFinished.load(); }

private:
    // 子任务计数归零且已 Complete → 回放回调栈（恰好一次，线程安全）。
    void MaybeFinish();

    CCallbackStack m_callbacks;
    std::atomic<int>  m_nPending;    // 未结束的子任务数。
    std::atomic<bool> m_bCompleted;  // 是否已 Complete。
    std::atomic<bool> m_bFinished;   // 是否已回放回调栈。
    std::mutex m_mutex;              // 串行化完成判定（防双重回放）。
};

} // namespace sc
