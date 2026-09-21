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

/// @brief 新建句柄（连同线程池对象 + 读写门：都带上本执行器的名字）。
///
/// 前提：`m_strName` 已经初始化（它在 `m_pHandle` 「之前」声明，所以构造时轮得到）。
///
/// @return 新句柄（线程池对象与读写门已创建，线程池未启动）。
std::shared_ptr<detail::CExecutorHandle> CAsyncExecutor::MakeHandle() const
{
    std::shared_ptr<detail::CExecutorHandle> pHandle(new detail::CExecutorHandle());
    pHandle->m_pPool.reset(new common::thread::CThreadPool(m_nThreadCount, m_strName));
    // 读写门绑在这条线程池上（池只执行、门只调度）；与池同寿命：门跟着句柄走。
    pHandle->m_pGate.reset(new CReadWriteGate(pHandle->m_pPool.get()));
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
/// 类别必填（没有默认写）： `kWrite` = 与模块内其它任务互斥，`kRead` = 可与其它读任务并发。
///
/// @param eKind 任务类别（读可并发 / 写独占）。
/// @param fnTask 任务函数。
/// @return true 已接受（已投递或在门口排队）；false 执行器已停止 / 未启动。
bool CAsyncExecutor::Post(TaskKind eKind, std::function<void()> fnTask)
{
    return PostImpl(eKind, std::move(fnTask));
}

/// @brief 投递实现：包异常兜底后按类别过读写门。
///
/// 包一层异常兜底的原因：线程池 worker 不捕获异常（异常逃出线程函数即 `std::terminate`），
/// 而这里投递的是「用户任务」，所以在框架边界上收口。
///
/// @param eKind 任务类别（读可并发 / 写独占 / 直投不过门）。
/// @param fnTask 任务函数（按值接收 + 移动投递，避免 std::function 拷贝）。
/// @return true 已接受（已投递或在门口排队）；false 执行器已停止 / 未启动 / 门已关闭。
bool CAsyncExecutor::PostImpl(TaskKind eKind, std::function<void()> fnTask)
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
    // 走与层派发同一个漏斗：读 / 写过门（未启动 / 已关闭 → false），`kDirect` 直投线程池。
    return detail::PostToHandle(m_pHandle, eKind, std::move(fnTaskGuarded));
}

/// @brief 停止并等待任务完成（优雅关闭）。
///
/// 顺序固定（反了会让门口排队的任务永远投不出去 —— 它们已经算「已接受」）：
///  ① 关读写门（拒新入队）→ ② 标记停止（`Post` / 层的快速拒绝路径）→ ③ 等门排空
///  （已接受的任务跑完）→ ④ 停线程池。
///
/// 注：先关门再标记，是为了让「看到 `IsStopped()` 为真」的调用方确定「门也已经关了」
/// （门是唯一权威的准入点，标记只是快速路径）—— 否则「刚标记、还没关门」的窗口里
/// 仍可能accept一个新层，停止语义就不可预测。
///
/// 保留句柄与线程池对象：已创建的 promise / 协程仍绑定本执行器句柄，停止后
/// 新投递被拒绝（对应层以「执行器已停」收口），不会访问已销毁对象。
void CAsyncExecutor::Stop()
{
    const std::shared_ptr<detail::CExecutorHandle>& pHandle = m_pHandle;
    if (pHandle->m_pGate != nullptr)
    {
        pHandle->m_pGate->Close();  // ① 门拒新（队列里已有的照旧跑完）。
    }
    pHandle->m_bStopped = true;  // ② 标记停止（Post / 层的快速拒绝路径）。
    if (pHandle->m_pGate != nullptr)
    {
        pHandle->m_pGate->Drain();  // ③ 等门口排队的任务跑完。
    }
    pHandle->m_pPool->Stop();  // ④ 线程池对象保留（回到未启动状态）。
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
