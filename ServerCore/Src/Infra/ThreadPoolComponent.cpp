#include "Infra/IThreadPool.h"

#include <string>

namespace sc {

/// @brief 创建线程池组件。
///
/// @param threadCount 工作线程数量。
CThreadPoolComponent::CThreadPoolComponent(size_t threadCount) : m_nThreadCount(threadCount)
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

/// @brief 提交任务。
///
/// @return 已启动时返回 true。
bool CThreadPoolComponent::Submit(const std::function<void()>& task)
{
    if (!m_pPool)
    {
        return false;
    }
    return m_pPool->Submit(task);
}

/// @brief 停止并等待任务完成。
void CThreadPoolComponent::Stop()
{
    if (m_pPool)
    {
        m_pPool->Stop();
        m_pPool.reset();
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
