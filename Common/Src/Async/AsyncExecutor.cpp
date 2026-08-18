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

/// @brief 启动工作线程（惰性创建 progschj 线程池）。
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
    pool_.reset(new ::ThreadPool(threadCount_));
    return true;
}

/// @brief 提交无返回值任务。
bool AsyncExecutor::Post(const std::function<void()>& task)
{
    if (!pool_)
    {
        return false;
    }
    pool_->enqueue(task);
    return true;
}

/// @brief 停止并等待任务完成。
void AsyncExecutor::Stop()
{
    pool_.reset(); // progschj 析构会 join 所有工作线程
}

/// @brief 是否正在运行。
bool AsyncExecutor::IsRunning() const
{
    return pool_ != nullptr;
}

} // namespace common
