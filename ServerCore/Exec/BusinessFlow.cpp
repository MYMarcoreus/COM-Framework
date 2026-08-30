#include "Exec/BusinessFlow.h"

namespace sc {

CBusinessFlow::CBusinessFlow()
    : m_nPending(0), m_bCompleted(false), m_bFinished(false)
{
}

CBusinessFlow::~CBusinessFlow()
{
}

/// @brief 提交一个读/写子任务到模块调度器（自动 BeginTask/EndTask）。
bool CBusinessFlow::SubmitTask(CModuleScheduler* pScheduler,
                               CModuleScheduler::ETaskKind eKind,
                               const std::function<void()>& fnTask)
{
    if (pScheduler == nullptr)
    {
        return false;
    }
    // 按值捕获流程自引用，保证子任务跨线程执行期间流程存活。
    std::shared_ptr<CBusinessFlow> spSelf = shared_from_this();
    BeginTask();
    bool bOk = pScheduler->Submit(eKind, [spSelf, fnTask]()
    {
        fnTask(); // 子任务业务逻辑（异常由调度器包装捕获）
        spSelf->EndTask();
    });
    if (!bOk)
    {
        EndTask(); // 投递失败：立即归还计数
        return false;
    }
    return true;
}

/// @brief 标记一个子任务开始。
void CBusinessFlow::BeginTask()
{
    m_nPending.fetch_add(1);
}

/// @brief 标记一个子任务结束（最后一个结束时尝试完成）。
void CBusinessFlow::EndTask()
{
    if (m_nPending.fetch_sub(1) == 1)
    {
        MaybeFinish();
    }
}

/// @brief 流程主体结束：不再投递新子任务；全部子任务排空后回放回调栈。
void CBusinessFlow::Complete()
{
    m_bCompleted.store(true);
    MaybeFinish();
}

/// @brief 子任务计数归零且已 Complete → 回放回调栈（恰好一次，线程安全）。
void CBusinessFlow::MaybeFinish()
{
    bool bShouldRun = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_bFinished.load())
        {
            return;
        }
        if (m_bCompleted.load() && m_nPending.load() == 0)
        {
            m_bFinished.store(true);
            bShouldRun = true;
        }
    }
    if (bShouldRun)
    {
        m_callbacks.RunAll(); // 处理结束：逐个出栈（LIFO）触发
    }
}

} // namespace sc
