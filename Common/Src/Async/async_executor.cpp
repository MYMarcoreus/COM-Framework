#include "Async/async_executor.h"

namespace common {

/// @brief 创建异步执行器。
///
/// @param threadCount 工作线程数量。
CAsyncExecutor::CAsyncExecutor(size_t threadCount) : threadCount_(threadCount)
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
    if (pool_)
    {
        return false;
    }
    if (threadCount_ == 0)
    {
        return false;
    }
    pool_.reset(new CThreadPool(threadCount_));
    return pool_->Start();
}

/// @brief 提交无返回值任务。
bool CAsyncExecutor::Post(const std::function<void()>& task)
{
    if (!pool_)
    {
        return false;
    }
    return pool_->Submit(task);
}

/// @brief 停止并等待任务完成。
void CAsyncExecutor::Stop()
{
    if (pool_)
    {
        pool_->Stop();
        pool_.reset();
    }
}

/// @brief 是否正在运行。
bool CAsyncExecutor::IsRunning() const
{
    return pool_ != nullptr && pool_->IsRunning();
}

} // namespace common
