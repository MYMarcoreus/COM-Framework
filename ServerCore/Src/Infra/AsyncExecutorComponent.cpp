#include "Infra/IAsyncExecutor.h"

#include <string>

namespace sc {

/// @brief 创建异步执行器组件。
///
/// @param nThreadCount 工作线程数量。
CAsyncExecutorComponent::CAsyncExecutorComponent(size_t nThreadCount) : m_nThreadCount(nThreadCount)
{
}

/// @brief 销毁异步执行器组件。
CAsyncExecutorComponent::~CAsyncExecutorComponent()
{
    Stop();
}

/// @brief 启动工作线程。
bool CAsyncExecutorComponent::Start()
{
    if (m_pExecutor)
    {
        return false;
    }
    if (m_nThreadCount == 0)
    {
        return false;
    }
    m_pExecutor.reset(new common::CAsyncExecutor(m_nThreadCount));
    return m_pExecutor->Start();
}

/// @brief 提交无返回值任务。
///
/// @return 已启动时返回 true。
bool CAsyncExecutorComponent::Post(const std::function<void()>& task)
{
    if (!m_pExecutor)
    {
        return false;
    }
    return m_pExecutor->Post(task);
}

/// @brief 停止并等待任务完成。
void CAsyncExecutorComponent::Stop()
{
    if (m_pExecutor)
    {
        m_pExecutor->Stop();
        m_pExecutor.reset();
    }
}

/// @brief 接口查询实现。
bool CAsyncExecutorComponent::QueryInterfaceImpl(const InterfaceId& iid, void** ppv)
{
    if (std::string(iid) == std::string(IID_IAsyncExecutor()))
    {
        *ppv = static_cast<IAsyncExecutor*>(this);
        return true;
    }
    return CComponent::QueryInterfaceImpl(iid, ppv);
}

} // namespace sc
