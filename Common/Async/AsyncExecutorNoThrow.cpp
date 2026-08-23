#include "Async/AsyncExecutorNoThrow.h"

namespace common {
namespace nothrow {

/// @brief 创建异步执行器（无异常版）。
///
/// @param nThreadCount 工作线程数量。
CAsyncExecutor::CAsyncExecutor(size_t nThreadCount) : m_nThreadCount(nThreadCount)
{
}

/// @brief 销毁异步执行器。
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

/// @brief 停止并等待任务完成。
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
