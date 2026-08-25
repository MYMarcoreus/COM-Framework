#include "Async/AsyncExecutorNoThrow.h"

namespace common {
namespace nothrow {

// ====================================================================
// 非模板成员定义（模板成员 Submit / CTask<TValue>::Then 等在头文件）。
// 非模板类 CAsyncExecutor 与显式特化 CTask<void> 的成员定义放本文件，
// 避免头文件中定义导致多 TU 重复定义（ODR 违规）。
// ====================================================================

/// @brief OnSuccess 实现（`CTask<void>` 特化）。
void CTask<void>::OnSuccess(const std::function<void()>& fnCallback)
{
    m_pState->AddContinuation(
        [fnCallback](const CTaskResult<void>& result)
        {
            if (result.HasValue() && fnCallback)
            {
                fnCallback();
            }
        });
}

/// @brief OnNone 实现（`CTask<void>` 特化）。
void CTask<void>::OnNone(const std::function<void(detail::CTaskEndReason)>& fnCallback)
{
    m_pState->AddContinuation(
        [fnCallback](const CTaskResult<void>& result)
        {
            if (!result.HasValue() && fnCallback)
            {
                fnCallback(result.Reason());
            }
        });
}

/// @brief 创建异步执行器。
CAsyncExecutor::CAsyncExecutor(size_t nThreadCount) : m_nThreadCount(nThreadCount)
{
}

/// @brief 销毁异步执行器（停止并等待任务完成）。
CAsyncExecutor::~CAsyncExecutor()
{
    Stop();
}

/// @brief 启动工作线程。
bool CAsyncExecutor::Start()
{
    if (m_pHandle)
    {
        return false;
    }
    if (m_nThreadCount == 0)
    {
        return false;
    }
    m_pHandle.reset(new detail::CExecutorHandle());
    m_pHandle->m_pPool.reset(new common::CThreadPool(m_nThreadCount));
    if (!m_pHandle->m_pPool->Start())
    {
        m_pHandle.reset();
        return false;
    }
    return true;
}

/// @brief 提交无返回值任务。
bool CAsyncExecutor::Post(const std::function<void()>& fnTask)
{
    std::shared_ptr<detail::CExecutorHandle> pHandle = m_pHandle;
    if (!pHandle || pHandle->m_bStopped || !pHandle->m_pPool)
    {
        return false;
    }
    return pHandle->m_pPool->Submit(fnTask);
}

/// @brief 停止并等待任务完成（优雅关闭）。
void CAsyncExecutor::Stop()
{
    std::shared_ptr<detail::CExecutorHandle> pHandle = m_pHandle;
    if (!pHandle)
    {
        return;
    }
    pHandle->m_bStopped = true;
    if (pHandle->m_pPool)
    {
        pHandle->m_pPool->Stop();
    }
    m_pHandle.reset();
}

/// @brief 是否正在运行。
bool CAsyncExecutor::IsRunning() const
{
    return m_pHandle != nullptr && !m_pHandle->m_bStopped &&
           m_pHandle->m_pPool != nullptr && m_pHandle->m_pPool->IsRunning();
}

} // namespace nothrow
} // namespace common
