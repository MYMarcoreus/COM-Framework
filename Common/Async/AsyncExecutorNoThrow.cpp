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
    if (m_pPool)
    {
        return false;
    }
    if (m_nThreadCount == 0)
    {
        return false;
    }
    m_pPool.reset(new common::CThreadPool(m_nThreadCount));
    return m_pPool->Start();
}

/// @brief 提交无返回值任务。
bool CAsyncExecutor::Post(const std::function<void()>& fnTask)
{
    if (!m_pPool)
    {
        return false;
    }
    return m_pPool->Submit(fnTask);
}

/// @brief 停止并等待任务完成。
void CAsyncExecutor::Stop()
{
    if (m_pPool)
    {
        m_pPool->Stop();
        m_pPool.reset();
    }
}

/// @brief 是否正在运行。
bool CAsyncExecutor::IsRunning() const
{
    return m_pPool != nullptr && m_pPool->IsRunning();
}

} // namespace nothrow
} // namespace common
