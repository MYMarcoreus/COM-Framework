#include "Async/AsyncExecutor.h"

namespace common {

/// @brief 创建异步执行器。
///
/// @param threadCount 工作线程数量。
AsyncExecutor::AsyncExecutor(size_t threadCount) : pool_(threadCount)
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
    return pool_.Start();
}

/// @brief 提交无返回值任务。
bool AsyncExecutor::Post(const std::function<void()>& task)
{
    return pool_.Submit(task);
}

/// @brief 停止并等待任务完成。
void AsyncExecutor::Stop()
{
    pool_.Stop();
}

/// @brief 是否正在运行。
bool AsyncExecutor::IsRunning() const
{
    return pool_.IsRunning();
}

} // namespace common
