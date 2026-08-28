#include "Infra/AsyncExecutorModule.h"

#include "Module/InterfaceMap.h"
#include <string>

namespace sc {

// 接口映射表：暴露本类实现的接口（查表驱动 QueryInterface）。
SC_DEFINE_INTERFACE_MAP(CAsyncExecutorModule, CModule, IAsyncExecutor)

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

/// @brief 初始化模块（无配置依赖，直接成功）。
bool CAsyncExecutorModule::Initialize(const CResolveContext& /*ctx*/)
{
    return true;
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
    m_pExecutor.reset(new common::async::CAsyncExecutor(m_nThreadCount));
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

/// @brief 关闭模块（执行器资源由 Stop / 析构释放，无需额外处理）。
void CAsyncExecutorModule::Shutdown()
{
}

} // namespace sc
