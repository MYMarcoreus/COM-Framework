#include "Infra/i_thread_pool.h"

#include <string>

namespace sc {

/// @brief 创建线程池组件。
///
/// @param threadCount 工作线程数量。
CThreadPoolComponent::CThreadPoolComponent(size_t threadCount) : threadCount_(threadCount)
{
}

/// @brief 销毁线程池组件。
CThreadPoolComponent::~CThreadPoolComponent()
{
    Stop();
}

/// @brief 启动工作线程。
bool CThreadPoolComponent::Start()
{
    if (pool_)
    {
        return false;
    }
    if (threadCount_ == 0)
    {
        return false;
    }
    pool_.reset(new common::CThreadPool(threadCount_));
    return pool_->Start();
}

/// @brief 提交任务。
///
/// @return 已启动时返回 true。
bool CThreadPoolComponent::Submit(const std::function<void()>& task)
{
    if (!pool_)
    {
        return false;
    }
    return pool_->Submit(task);
}

/// @brief 停止并等待任务完成。
void CThreadPoolComponent::Stop()
{
    if (pool_)
    {
        pool_->Stop();
        pool_.reset();
    }
}

/// @brief 接口查询实现。
bool CThreadPoolComponent::QueryInterfaceImpl(const InterfaceId& iid, void** ppv)
{
    if (std::string(iid) == std::string(IID_IThreadPool()))
    {
        *ppv = static_cast<IThreadPool*>(this);
        return true;
    }
    return CComponent::QueryInterfaceImpl(iid, ppv);
}

} // namespace sc
