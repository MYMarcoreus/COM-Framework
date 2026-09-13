#include "Async/AsyncExecutor.h"

// ====================================================================
// 非模板成员定义（模板成员 NewPromise / CoStart / 组合器分别在
// Promise.h / Coroutine/Coroutine.h / 本头文件的组合器一节内定义）。非模板类 CAsyncExecutor 的
// 成员定义放本文件，避免头文件中定义导致多 TU 重复定义（ODR 违规）。
//
// 注：诊断钩子（SetDiagnosticHandler / ReportDiagnostic）已独立到 Async/Diagnostics.{h,cpp}。
// ====================================================================

namespace common {
namespace async {

/// @brief 创建异步执行器（构造即建句柄与线程池对象：执行器和线程池一定不为空）。
///
/// 线程池对象始终存在但未启动；须显式 Start() 后工作线程才开始取任务。
///
/// @param nThreadCount 工作线程数。
CAsyncExecutor::CAsyncExecutor(size_t nThreadCount) : CAsyncExecutor(std::string(), nThreadCount)
{}
/// @brief 创建具名异步执行器（名字只用于调试：线程名 + trace 里看得到）。
///
/// @param strName 执行器名（空 = 未命名）。
/// @param nThreadCount 工作线程数。
CAsyncExecutor::CAsyncExecutor(const std::string& strName, size_t nThreadCount)
    : m_strName(strName),  // 必须排在 m_pHandle 之前（声明序即初始化序；MakeHandle 要用名字）。
      m_nThreadCount(nThreadCount),
      m_pHandle(MakeHandle())
{}

/// @brief 销毁异步执行器（停止线程池并等待已投递任务完成）。
CAsyncExecutor::~CAsyncExecutor()
{
    Stop();
}

/// @brief 新建句柄（连同线程池对象：都带上本执行器的名字）。
///
/// 前提：`m_strName` 已经初始化（它在 `m_pHandle` **之前**声明，所以构造时轮得到）。
///
/// @return 新句柄（线程池对象已创建但未启动）。
std::shared_ptr<detail::CExecutorHandle> CAsyncExecutor::MakeHandle() const
{
    std::shared_ptr<detail::CExecutorHandle> pHandle(new detail::CExecutorHandle());
    pHandle->m_pPool.reset(new common::thread::CThreadPool(m_nThreadCount, m_strName));
    return pHandle;
}

/// @brief 启动工作线程。
///
/// @return true 启动成功；false 已启动或线程数为 0。
bool CAsyncExecutor::Start()
{
    if (m_pHandle->m_pPool->IsRunning())
    {
        return false;  // 已启动。
    }
    if (m_nThreadCount == 0)
    {
        return false;
    }
    // 若之前 Stop 过（句柄已标记停止），重建句柄与线程池以隔离旧任务。
    if (m_pHandle->m_bStopped)
    {
        m_pHandle = MakeHandle();  // 重建：句柄、名字、线程池一起换。
    }
    return m_pHandle->m_pPool->Start();
}

/// @brief 投递无返回值任务（fire-and-forget）。
///
/// 按值接收 + 移动投递，避免 std::function 拷贝。
///
/// @param fnTask 任务函数。
/// @return true 提交成功；false 执行器已停止 / 线程池未启动。
bool CAsyncExecutor::Post(std::function<void()> fnTask)
{
    // 直接解引用句柄（不复制 shared_ptr），避免每次提交时原子引用计数
    // 在高并发下争抢同一 cache line。
    if (m_pHandle->m_bStopped)
    {
        return false;
    }
    if (!fnTask)
    {
        ReportDiagnostic(detail::kDiagPostEmpty);
        return false;
    }

    // 包一层异常兜底：线程池 worker 不捕获异常（异常逃出线程函数即 std::terminate），
    // 而 Post 投递的是**用户任务**，所以在框架边界上收口。
    std::function<void()> fnTaskGuarded = [fnTask]()
    {
        try
        {
            fnTask();
        }
        catch (...)
        {
            ReportDiagnostic(detail::kDiagPostThrow);
        }
    };
    return m_pHandle->m_pPool->Submit(std::move(fnTaskGuarded));  // 移动投递；未启动 → false。
}

/// @brief 停止并等待任务完成（优雅关闭）。
///
/// 保留句柄与线程池对象：已创建的 promise / 协程仍绑定本执行器句柄，停止后
/// 新投递被拒绝（对应层以 kStopped 被拒绝），不会访问已销毁对象。
void CAsyncExecutor::Stop()
{
    const std::shared_ptr<detail::CExecutorHandle>& pHandle = m_pHandle;
    pHandle->m_bStopped = true;
    pHandle->m_pPool->Stop();  // 线程池对象保留（回到未启动状态）。
}

/// @brief 是否正在运行。
bool CAsyncExecutor::IsRunning() const
{
    return !m_pHandle->m_bStopped && m_pHandle->m_pPool->IsRunning();
}

/// @brief 是否已停止（轻量原子读，不加锁）。
bool CAsyncExecutor::IsStopped() const
{
    return m_pHandle->m_bStopped.load(std::memory_order_relaxed);
}

}  // namespace async
}  // namespace common
