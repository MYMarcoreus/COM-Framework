#include "Async/AsyncExecutor.h"

namespace common {

/// @brief 创建异步执行器。
///
/// @param threadCount 工作线程数量。
AsyncExecutor::AsyncExecutor(size_t threadCount) : threadCount_(threadCount)
{
}

/// @brief 销毁异步执行器。
AsyncExecutor::~AsyncExecutor()
{
    Stop();
}

/// @brief 启动工作线程。
bool AsyncExecutor::Start()
{
    if (pool_)
    {
        return false;
    }
    if (threadCount_ == 0)
    {
        return false;
    }
    pool_.reset(new ThreadPool(threadCount_));
    return pool_->Start();
}

/// @brief 提交无返回值任务。
bool AsyncExecutor::Post(const std::function<void()>& task)
{
    if (!pool_)
    {
        return false;
    }
    return pool_->Submit(task);
}

/// @brief 停止并等待任务完成。
void AsyncExecutor::Stop()
{
    if (pool_)
    {
        pool_->Stop();
        pool_.reset();
    }
}

/// @brief 是否正在运行。
bool AsyncExecutor::IsRunning() const
{
    return pool_ != nullptr && pool_->IsRunning();
}

} // namespace common
