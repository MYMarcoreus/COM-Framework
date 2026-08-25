#include "Async/AsyncExecutorNoThrow.h"

namespace common {
namespace nothrow {

// ====================================================================
// 非模板成员定义（模板成员 Submit / CTask<TValue>::Then 等在头文件）。
// 非模板类 CAsyncExecutor 与显式特化 CTask<void> 的成员定义放本文件，
// 避免头文件中定义导致多 TU 重复定义（ODR 违规）。
// ====================================================================

/// @brief OnSuccess 实现（`CTask<void>` 特化）。
bool CTask<void>::OnSuccess(const std::function<void()>& fnCallback)
{
    return m_pState->AddContinuation(this->m_pExecutor,
        [fnCallback](const CTaskResult<void>& result)
        {
            if (result.HasValue() && fnCallback)
            {
                fnCallback();
            }
        });
}

/// @brief OnNone 实现（`CTask<void>` 特化）。
bool CTask<void>::OnNone(const std::function<void(detail::CTaskEndReason)>& fnCallback)
{
    return m_pState->AddContinuation(this->m_pExecutor,
        [fnCallback](const CTaskResult<void>& result)
        {
            if (!result.HasValue() && fnCallback)
            {
                fnCallback(result.Reason());
            }
        });
}

/// @brief 创建异步执行器（构造即建句柄与线程池对象：执行器和线程池一定不为空）。
CAsyncExecutor::CAsyncExecutor(size_t nThreadCount)
    : m_pHandle(new detail::CExecutorHandle()), // 句柄（线程池对象随即创建）。
      m_nThreadCount(nThreadCount)
{
    m_pHandle->m_pPool.reset(new common::CThreadPool(m_nThreadCount)); // 线程池对象总在（未启动）。
}

/// @brief 销毁异步执行器（停止并等待任务完成）。
CAsyncExecutor::~CAsyncExecutor()
{
    Stop();
}

/// @brief 启动工作线程。
bool CAsyncExecutor::Start()
{
    if (m_pHandle->m_pPool->IsRunning())
    {
        return false; // 已启动
    }
    if (m_nThreadCount == 0)
    {
        return false;
    }
    // 若之前 Stop 过（句柄已标记停止），重建句柄与线程池以隔离旧任务。
    if (m_pHandle->m_bStopped)
    {
        m_pHandle.reset(new detail::CExecutorHandle());
        m_pHandle->m_pPool.reset(new common::CThreadPool(m_nThreadCount));
    }
    if (!m_pHandle->m_pPool->Start())
    {
        return false;
    }
    return true;
}

/// @brief 提交无返回值任务。
bool CAsyncExecutor::Post(const std::function<void()>& fnTask)
{
    std::shared_ptr<detail::CExecutorHandle> pHandle = m_pHandle;
    if (pHandle->m_bStopped)
    {
        return false;
    }
    return pHandle->m_pPool->Submit(fnTask); // 未启动 → false（投递失败）。
}

/// @brief 停止并等待任务完成（优雅关闭）。
///
/// 保留句柄与线程池对象：已创建任务仍绑定本执行器（m_bStopped=true，不再异步投递）。
void CAsyncExecutor::Stop()
{
    std::shared_ptr<detail::CExecutorHandle> pHandle = m_pHandle;
    pHandle->m_bStopped = true;
    pHandle->m_pPool->Stop(); // 线程池对象保留（未启动状态）。
}

/// @brief 是否正在运行。
bool CAsyncExecutor::IsRunning() const
{
    return !m_pHandle->m_bStopped && m_pHandle->m_pPool->IsRunning();
}

} // namespace nothrow
} // namespace common
