#include "Infra/IAsyncExecutor.h"

#include <string>

namespace sc {

/// @brief 创建异步执行器组件。
///
/// @param threadCount 工作线程数量。
AsyncExecutorComponent::AsyncExecutorComponent(size_t threadCount) : threadCount_(threadCount)
{
}

/// @brief 销毁异步执行器组件。
AsyncExecutorComponent::~AsyncExecutorComponent()
{
    Stop();
}

/// @brief 启动工作线程。
bool AsyncExecutorComponent::Start()
{
    if (executor_)
    {
        return false;
    }
    if (threadCount_ == 0)
    {
        return false;
    }
    executor_.reset(new common::AsyncExecutor(threadCount_));
    return executor_->Start();
}

/// @brief 提交无返回值任务。
///
/// @return 已启动时返回 true。
bool AsyncExecutorComponent::Post(const std::function<void()>& task)
{
    if (!executor_)
    {
        return false;
    }
    return executor_->Post(task);
}

/// @brief 停止并等待任务完成。
void AsyncExecutorComponent::Stop()
{
    if (executor_)
    {
        executor_->Stop();
        executor_.reset();
    }
}

/// @brief 接口查询实现。
bool AsyncExecutorComponent::QueryInterfaceImpl(const InterfaceId& iid, void** ppv)
{
    if (std::string(iid) == std::string(IID_IAsyncExecutor()))
    {
        *ppv = static_cast<IAsyncExecutor*>(this);
        return true;
    }
    return Component::QueryInterfaceImpl(iid, ppv);
}

} // namespace sc
