#include "Infra/ThreadPoolModule.h"

#include "Module/InterfaceMap.h"
#include <string>

namespace sc {

// 接口映射表：暴露本类实现的接口（查表驱动 QueryInterface）。
SC_DEFINE_INTERFACE_MAP(CThreadPoolModule, CModule, IThreadPool)

/// @brief 创建线程池模块。
///
/// @param nThreadCount 工作线程数量。
CThreadPoolModule::CThreadPoolModule(size_t nThreadCount)
    : CModule("thread-pool"), m_nThreadCount(nThreadCount)
{
}

/// @brief 销毁线程池模块。
CThreadPoolModule::~CThreadPoolModule()
{
    Stop();
}

/// @brief 初始化模块（无配置依赖，直接成功）。
bool CThreadPoolModule::Initialize(const CResolveContext& /*ctx*/)
{
    return true;
}

/// @brief 启动工作线程。
bool CThreadPoolModule::Start()
{
    if (m_pPool)
    {
        return false;
    }
    if (m_nThreadCount == 0)
    {
        return false;
    }
    m_pPool.reset(new common::thread::CThreadPool(m_nThreadCount));
    return m_pPool->Start();
}

/// @brief 提交任务。
///
/// @return 已启动时返回 true。
bool CThreadPoolModule::Submit(const std::function<void()>& task)
{
    if (!m_pPool)
    {
        return false;
    }
    return m_pPool->Submit(task);
}

/// @brief 停止并等待任务完成。
void CThreadPoolModule::Stop()
{
    if (m_pPool)
    {
        m_pPool->Stop();
        m_pPool.reset();
    }
}

/// @brief 关闭模块（线程池资源由 Stop / 析构释放，无需额外处理）。
void CThreadPoolModule::Shutdown()
{
}

} // namespace sc
