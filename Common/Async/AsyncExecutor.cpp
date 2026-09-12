#include "Async/AsyncExecutor.h"

#include <cstdio>
#include <mutex>

// ====================================================================
// 非模板成员定义（模板成员 NewPromise / CoStart 分别在 Promise.h /
// Coroutine.h 内定义）。非模板类 CAsyncExecutor 的成员定义放本文件，
// 避免头文件中定义导致多 TU 重复定义（ODR 违规）。
// ====================================================================

namespace common {
namespace async {

namespace {

/// @brief 诊断处理器槽（进程级；进程内共享一个，故加锁保护）。
std::mutex& DiagnosticMutex()
{
    static std::mutex s_mutex;
    return s_mutex;
}

/// @brief 当前诊断处理器（空 = 未设置，走默认策略）。
DiagnosticHandler& DiagnosticSlot()
{
    static DiagnosticHandler s_fnHandler;
    return s_fnHandler;
}

}  // namespace

/// @brief 设置诊断处理器（线程安全）。
///
/// @param fnHandler 处理器；传 nullptr 恢复默认（debug 构建打印到 stderr，发布构建忽略）。
void SetDiagnosticHandler(const DiagnosticHandler& fnHandler)
{
    std::lock_guard<std::mutex> lock(DiagnosticMutex());
    DiagnosticSlot() = fnHandler;
}

/// @brief 报告一次诊断（框架内部用；没设处理器时按默认策略处理，自身不抛异常）。
///
/// @param strWhat 问题描述。
void ReportDiagnostic(const char* strWhat)
{
    DiagnosticHandler fnHandler;
    {
        std::lock_guard<std::mutex> lock(DiagnosticMutex());
        fnHandler = DiagnosticSlot();
    }

    if (fnHandler)
    {
        try
        {
            fnHandler(strWhat != nullptr ? strWhat : "(null)");
        }
        catch (...)
        {
            // 诊断处理器自己抛异常：忽略（报告问题的手段不能反过来弄坏框架）。
        }
        return;
    }

#if !defined(NDEBUG)
    // 默认策略：debug 构建打印（让开发期一眼看到误用），发布构建安静。
    std::fprintf(stderr, "[async] %s\n", strWhat != nullptr ? strWhat : "(null)");
#endif
}

/// @brief 创建异步执行器（构造即建句柄与线程池对象：执行器和线程池一定不为空）。
///
/// 线程池对象始终存在但未启动；须显式 Start() 后工作线程才开始取任务。
///
/// @param nThreadCount 工作线程数。
CAsyncExecutor::CAsyncExecutor(size_t nThreadCount)
    : m_pHandle(new detail::CExecutorHandle()),  // 句柄（线程池对象随即创建）。
      m_nThreadCount(nThreadCount)
{
    m_pHandle->m_pPool.reset(new common::thread::CThreadPool(m_nThreadCount));  // 线程池对象总在（未启动）。
}

/// @brief 销毁异步执行器（停止线程池并等待已投递任务完成）。
CAsyncExecutor::~CAsyncExecutor()
{
    Stop();
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
        m_pHandle.reset(new detail::CExecutorHandle());
        m_pHandle->m_pPool.reset(new common::thread::CThreadPool(m_nThreadCount));
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

/// @brief 线程池是否空闲（无排队任务；协程内联续接判断用）。
bool CAsyncExecutor::IsIdle() const
{
    return m_pHandle->m_pPool->PendingCount() == 0;
}

}  // namespace async
}  // namespace common
