#include "Async/AsyncExecutor.h"

namespace common {

/// @brief 创建异步执行器。
///
/// @param threadCount 工作线程数量。
CAsyncExecutor::CAsyncExecutor(size_t threadCount) : m_nThreadCount(threadCount)
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
    m_pPool.reset(new CThreadPool(m_nThreadCount));
    return m_pPool->Start();
}

/// @brief 提交无返回值任务。
bool CAsyncExecutor::Post(const std::function<void()>& task)
{
    if (!m_pPool)
    {
        return false;
    }
    return m_pPool->Submit(task);
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

} // namespace common
