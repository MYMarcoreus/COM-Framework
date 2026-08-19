#include "Infra/IAsyncExecutor.h"

#include <string>

namespace sc {

/// @brief 创建异步执行器模块。
///
/// @param nThreadCount 工作线程数量。
CAsyncExecutorModule::CAsyncExecutorModule(size_t nThreadCount)
    : CModule("async-executor"), m_nThreadCount(nThreadCount)
{
}

/// @brief 销毁异步执行器模块。
CAsyncExecutorModule::~CAsyncExecutorModule()
{
    Stop();
}

/// @brief 启动工作线程。
bool CAsyncExecutorModule::Start()
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
bool CAsyncExecutorModule::Post(const std::function<void()>& task)
{
    if (!m_pExecutor)
    {
        return false;
    }
    return m_pExecutor->Post(task);
}

/// @brief 停止并等待任务完成。
void CAsyncExecutorModule::Stop()
{
    if (m_pExecutor)
    {
        m_pExecutor->Stop();
        m_pExecutor.reset();
    }
}

/// @brief 接口查询实现。
bool CAsyncExecutorModule::QueryInterfaceImpl(const InterfaceId& iid, void** ppv)
{
    if (iid == IID_IAsyncExecutor())
    {
        *ppv = static_cast<IAsyncExecutor*>(this);
        return true;
    }
    return CModule::QueryInterfaceImpl(iid, ppv);
}

} // namespace sc
