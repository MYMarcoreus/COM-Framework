#include "Async/ReadWriteGate.h"

#include <utility>

#include "Assert.h"
#include "Async/Diagnostics.h"
#include "Thread/ThreadPool.h"

namespace common {
namespace async {

namespace detail {

/// @brief 读写门侧诊断文案（集中一处：测试断言常量，而不是去匹配子串）。
///
/// @note 定义在本文件：包装任务已整体移到 .cpp，头文件不必再认识 `ReportDiagnostic`。
constexpr const char* kDiagGateThrow = "读写门：任务抛出了异常（已兜住，未终止进程）";

/// @brief 投递被池拒（停放竞态）→ 任务被丢弃时的诊断（区别于「任务体抛异常」）。
constexpr const char* kDiagGateTaskDropped = "读写门：线程池已停，任务被丢弃（须遵守 Close → Drain → 停线程池）";

}  // namespace detail

//================ Lifecycle ================

/// @brief 创建读写门。
///
/// @param pPool 线程池（只执行不调度；生命周期由调用方保证不短于门）。
/// @param nMaxReaders 最大并发读任务数（0 = 不设上限，仅受线程池线程数约束）。
CReadWriteGate::CReadWriteGate(common::thread::CThreadPool* pPool, size_t nMaxReaders)
    : m_pPool(pPool),
      m_nMaxReaders(nMaxReaders),
      m_nActiveReaders(0),
      m_bWriterActive(false),
      m_bClosed(false),
      m_nDrainWaiters(0)
{
    ASSERT(m_pPool != nullptr);  // 组合契约：门必须绑在一条线程池上（执行器组合时必然给出）。
}

/// @brief 关闭本门：拒绝新的提交。
///
/// 只影响之后的 `Submit`（返回 `false`）；已经入队 / 已经在跑的任务照旧执行完 ——
/// 与执行器「停了的执行器不再跑新层」的语义对齐。
void CReadWriteGate::Close()
{
    m_bClosed.store(true);
}

/// @brief 等待排空（无活跃任务、无排队任务才返回）。
///
/// @note 建议先 `Close()` 再 `Drain()`：Drain 不阻止新提交，持续投递会让它等不到排空。
/// @warning 排空依赖线程池仍在运行 —— 先停池再 Drain 会永久等待。
///          （调用方契约：`Close()` → `Drain()` → 停线程池。）
///
/// 等待期间会登记「有等待者」计数，任务结束那一侧据此决定要不要发通知
/// （没人等就不发 —— 那是一次每任务都要付的共享状态写）。
void CReadWriteGate::Drain()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    ++m_nDrainWaiters;  // 登记在锁内：发通知的一方也在锁内读它，不会漏唤醒。
    m_condition.wait(lock,
        [this]()
        {
            return IsIdleLocked();
        });
    --m_nDrainWaiters;
}

//================ Submit ================

/// @brief 提交任务（线程安全；严格按提交顺序入队，能放行则立即投递到线程池）。
///
/// 非阻塞：进不了模块的任务留在队列（不占用调用线程，也不占用线程池线程），
/// 等槽位释放（`OnTaskExit`）时再按序投递。
///
/// @param eKind 任务类别（读可并发 / 写独占）。
/// @param fnTask 任务体（移动投递；空任务视为编程错误）。
/// @return
///     true  已接受（已入队：可能已投递，也可能在排队）；
///     false 未接受（门已关闭 / 线程池未运行）。
bool CReadWriteGate::Submit(TaskKind eKind, std::function<void()> fnTask)
{
    ASSERT(fnTask);  // 空任务：调用方不该提交（执行器侧已在入口拦下）。
    ASSERT_MSG(eKind != TaskKind::kDirect, "kDirect 不过门（由执行器直投线程池）");  // 门对直投任务完全不知情。

    // ① 拒绝路径：门已关闭 / 线程池不可用 → 未接受（调用方按「执行器不可用」收口）。
    if (m_bClosed.load() || !m_pPool->IsRunning())
    {
        return false;
    }

    // ② 统一入队（严格按提交顺序），随后在锁内尝试从队首放行。
    std::vector<CWrappedTask>& vecDispatch = ScratchDispatchBuffer();
    vecDispatch.clear();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_dequeTasks.push_back(CWrappedTask(this, eKind, std::move(fnTask)));
        PumpLocked(vecDispatch);
    }

    // ③ 已取得槽位的任务在锁外投递（投递动作不得持门锁：池的任务回调会回来拿它）。
    DispatchToPool(vecDispatch);
    return true;
}

//================ Inline ================

/// @brief 能否「就地」跑本层（不投递、直接在当前线程继续）。
///
/// 两个条件（都在本函数里判定，不在调用方复制规则）：
///  - 本线程正跑着「本门 + 同类」的任务：那时槽位已经在手，就地跑不新增占用 ——
///    帧由「过门投递的任务」压上，就地跑下来的层沿用外层帧（同类）；
///  - 无人在排队：有人排队就让路（就地会插队，破坏公平）。
///
/// @param eKind 本层类别。
/// @return true 可以就地执行（调用方直接跑任务体即可，无需占位 / 归还）。
bool CReadWriteGate::CanRunInline(TaskKind eKind) const
{
    ASSERT_MSG(eKind != TaskKind::kDirect, "kDirect 不过门（CanRunInline 对它无意义）");  // 与 Submit 同一契约。
    // ① 手上的槽位是不是「本门 + 同类」（不在任务里 / 别的门 / 别的类别 → 都不能就地）。
    const detail::CTaskFrame* pFrame = detail::CTaskFrame::Top();
    if (pFrame == nullptr || pFrame->pGate != this || pFrame->eKind != eKind)
    {
        return false;
    }

    // ② 无人排队才让就地（先来后到：就地任务不得越过队列里的任务）。
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_dequeTasks.empty();
}

//================ Internal ================

/// @brief 构造（移动任务体）。
CReadWriteGate::CWrappedTask::CWrappedTask(CReadWriteGate* pGate, TaskKind eKind, std::function<void()> fnTask)
    : pGate(pGate), eKind(eKind), fnTask(std::move(fnTask))
{}

/// @brief 移动构造（`std::function` 存放目标时用它，避免拷贝任务体）。
CReadWriteGate::CWrappedTask::CWrappedTask(CWrappedTask&& other)
    : pGate(other.pGate), eKind(other.eKind), fnTask(std::move(other.fnTask))
{}

/// @brief 拷贝构造（只为满足 `std::function` 目标的 CopyConstructible 要求）。
///
/// @note 实际不会走到：投递全程是移动（`Submit(CTask&&)` → 池队列移动存储）。
CReadWriteGate::CWrappedTask::CWrappedTask(const CWrappedTask& other)
    : pGate(other.pGate), eKind(other.eKind), fnTask(other.fnTask)
{}

/// @brief 执行任务体（压「当前任务」帧 → 兜异常 → 归还槽位）。
///
/// 归还槽位放在异常兜底之后（不在 `try` 里）：任务体抛异常也必须归还 ——
/// 否则写者标志回不来，模块永久卡死。
void CReadWriteGate::CWrappedTask::operator()()
{
    const detail::CTaskFrame frame(pGate, eKind);  // 跑完自动弹帧（异常路径也弹）。
    try
    {
        fnTask();
    }
    catch (...)
    {
        ReportDiagnostic(detail::kDiagGateThrow);
    }
    pGate->OnTaskExit(eKind);  // 契约：不抛（见头文件 `operator()` 说明）。
}

/// @brief 放行收集缓冲（线程局部复用）。
///
/// 为什么复用：`Submit` / `OnTaskExit` 每次都要收集「本轮放行的任务」，临时 vector
/// 会给每次投递带来一次堆分配。缓冲是线程局部的，容量在第一次用满后不再增长。
///
/// 安全前提：只在「锁内收集 → 锁外投递 → 清空」这一段里使用；投递动作只入队、
/// 不同步回调（线程池不在这里跑任务），所以同一线程上不会嵌套使用它。
///
/// @return 本线程的收集缓冲（调用方负责先清空、用完由 `DispatchToPool` 清空）。
std::vector<CReadWriteGate::CWrappedTask>& CReadWriteGate::ScratchDispatchBuffer()
{
    static thread_local std::vector<CWrappedTask> s_vecDispatch;
    return s_vecDispatch;
}

/// @brief 空闲判定（持锁调用）。
///
/// @return true 无活跃任务、无排队任务。
bool CReadWriteGate::IsIdleLocked() const
{
    return m_nActiveReaders.load() == 0 && !m_bWriterActive.load() && m_dequeTasks.empty();
}

/// @brief 归还槽位（持锁调用）。
///
/// @param eKind 刚结束 / 刚被丢弃任务的类别（决定归还哪个槽位）。
void CReadWriteGate::ReturnSlot(TaskKind eKind)
{
    if (eKind == TaskKind::kRead)
    {
        m_nActiveReaders.fetch_sub(1);
    }
    else
    {
        m_bWriterActive.store(false);
    }
}

/// @brief 唤醒 `Drain()` 的等待方（持锁调用）。
///
/// 只在真的有人在等时才 `notify_all()`：99.9% 的运行里没有等待者，
/// 而 `notify_all` 会写共享状态（条件变量的内部计数）—— 在「提交线程与 worker 并发」
/// 的窗口里，这种每任务一次的共享写要花到几十 ns。
void CReadWriteGate::NotifyDrainWaitersLocked()
{
    if (m_nDrainWaiters != 0)
    {
        m_condition.notify_all();
    }
}

/// @brief 公平 FIFO：从队首顺序放行可准入的任务（持锁调用）。
///
/// 放行规则：
///  - 队首为读：无写者活跃且读槽位未满 → 放行；继续看下一个（读可并发）；
///  - 队首为写：无活跃读者 → 放行（独占）；否则停住 —— 其后的读/写一律等待，
///    既保证写不越过先前提交的任务，也保证写不被其后提交的读插队。
///
/// @param vecDispatch 输出：本轮取得槽位的任务（调用方锁外投递，可能一次放行多个读）。
void CReadWriteGate::PumpLocked(std::vector<CWrappedTask>& vecDispatch)
{
    while (!m_dequeTasks.empty())
    {
        // 仅读队首的类别做判定（不持有引用：下面 pop_front 会让引用悬垂）。
        const TaskKind eFront = m_dequeTasks.front().eKind;
        if (eFront == TaskKind::kRead)
        {
            if (m_bWriterActive.load())
            {
                break;  // 写者独占中。
            }
            if (m_nMaxReaders > 0 && m_nActiveReaders.load() >= m_nMaxReaders)
            {
                break;  // 读槽位已满。
            }
            // 先移动到局部再出队，避免移动已弹出的元素。
            CWrappedTask entry = std::move(m_dequeTasks.front());
            m_dequeTasks.pop_front();
            m_nActiveReaders.fetch_add(1);
            vecDispatch.push_back(std::move(entry));
        }
        else  // kWrite
        {
            if (m_bWriterActive.load())
            {
                break;  // 已有写者。
            }
            if (m_nActiveReaders.load() > 0)
            {
                break;  // 等先前读者排空（其后的任务一并等待，不越过该写）。
            }
            CWrappedTask entry = std::move(m_dequeTasks.front());
            m_dequeTasks.pop_front();
            m_bWriterActive.store(true);
            vecDispatch.push_back(std::move(entry));
            break;  // 写者独占：其后任务待写完成。
        }
    }
}

/// @brief 任务结束：归还槽位并继续泵出（在线程池线程中调用）。
///
/// 归还与泵出在同一临界区内完成，保证「槽位一空出来就有人接手」不会丢窗口；
/// 唤醒 `Drain()` 的等待方（**仅当真的有等待者**）；新放行的任务在锁外投递。
///
/// @param eKind 刚结束任务的类别（决定归还哪个槽位）。
void CReadWriteGate::OnTaskExit(TaskKind eKind)
{
    std::vector<CWrappedTask>& vecDispatch = ScratchDispatchBuffer();
    vecDispatch.clear();
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // ① 归还槽位。
        ReturnSlot(eKind);

        // ② 槽位空出 → 按序放行后续任务（可能一次放行多个读）。
        PumpLocked(vecDispatch);
        NotifyDrainWaitersLocked();
    }

    // ③ 锁外投递（与 Submit 同一约定：投递动作不持门锁）。
    DispatchToPool(vecDispatch);
}

/// @brief 把已取得槽位的任务投递到线程池（锁外调用）。
///
/// 正常情况下这里只有一种结局：池在跑 → 收下（「跑任务体 + 归还槽位」整个对象移给池）。
///
/// 唯一的失败是「池已进入停放（`m_bStopping`）」的竞态（调用方违反 Close → Drain → 停池）：
/// 这时**丢弃该任务**并归还槽位、报诊断。为什么不能留回队列里以后再说 ——
/// 任务体在构造 `Submit` 形参时就被移走了（拿不回来），而且池不会再跑任务，
/// 留在队列里也没有谁会泵出它；而伪装的「任务体抛异常」诊断会把真正的问题藏起来。
///
/// @param vecDispatch 本轮取得槽位的任务（处理完会被清空）。
void CReadWriteGate::DispatchToPool(std::vector<CWrappedTask>& vecDispatch)
{
    bool bDroppedAny = false;
    for (size_t i = 0; i < vecDispatch.size(); ++i)
    {
        const TaskKind eKind = vecDispatch[i].eKind;  // 先记类别：下面会把整个对象移走。
        if (m_pPool->Submit(std::move(vecDispatch[i])))
        {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ReturnSlot(eKind);
            NotifyDrainWaitersLocked();  // 槽位回来了 → 可能已空闲，唤醒等 Drain() 的人。
        }
        bDroppedAny = true;
    }
    vecDispatch.clear();

    // 诊断在锁外报（处理器是用户代码：它若反过来查门 —— PendingCount() 等 —— 持锁就会自锁）；
    // 一轮只报一次（这是「调用方违约」的信号，不是每任务一条的日志）。
    if (bDroppedAny)
    {
        ReportDiagnostic(detail::kDiagGateTaskDropped);
    }
}

//================ Query ================

/// @brief 是否已关闭。
///
/// @return 已调用过 `Close()` 返回 true。
bool CReadWriteGate::IsClosed() const
{
    return m_bClosed.load();
}

/// @brief 是否空闲（无活跃任务、无排队任务）。
///
/// @return true 空闲。
bool CReadWriteGate::IsIdle() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return IsIdleLocked();
}

/// @brief 排队中的任务数（持锁统计）。
///
/// @return 尚未取得槽位的任务数（不含已在跑的任务）。
size_t CReadWriteGate::PendingCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_dequeTasks.size();
}

/// @brief 当前活跃（已放行、未结束）的读任务数。
///
/// @return 活跃读任务数（原子读，近似值，仅诊断用）。
size_t CReadWriteGate::ActiveReaders() const
{
    return m_nActiveReaders.load();
}

/// @brief 当前是否有写任务在跑。
///
/// @return true 有写任务在跑（原子读，近似值，仅诊断用）。
bool CReadWriteGate::HasActiveWriter() const
{
    return m_bWriterActive.load();
}

}  // namespace async
}  // namespace common
