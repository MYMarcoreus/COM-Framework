#include "Async/AsyncExecutor.h"

// ====================================================================
// 非模板成员定义（模板成员 Submit / CoStart 分别在 AsyncChain.h /
// Coroutine.h 内定义）。非模板类 CAsyncExecutor 的成员定义放本文件，
// 避免头文件中定义导致多 TU 重复定义（ODR 违规）。
// ====================================================================

namespace common {
namespace async {

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
    return m_pHandle->m_pPool->Submit(std::move(fnTask));  // 移动投递；未启动 → false。
}

/// @brief 停止并等待任务完成（优雅关闭）。
///
/// 保留句柄与线程池对象：已创建的链 / 协程仍绑定本执行器句柄，停止后
/// 新投递被拒绝（对应层以 kStepStopped 失败），不会访问已销毁对象。
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
