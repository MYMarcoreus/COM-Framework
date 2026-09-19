#include "Async/ReadWriteGate.h"

#include <utility>

#include "Assert.h"
#include "Async/Diagnostics.h"

namespace common {
namespace async {

// #region Lifecycle

//================ Lifecycle ================

/// @brief 创建读写门。
///
/// @param pPool 线程池（只执行不调度；生命周期由调用方保证不短于门）。
/// @param nMaxReaders 最大并发读任务数（0 = 不设上限，仅受线程池线程数约束）。
CReadWriteGate::CReadWriteGate(common::thread::CThreadPool* pPool, size_t nMaxReaders)
    : m_pPool(pPool), m_nMaxReaders(nMaxReaders), m_nActiveReaders(0), m_bWriterActive(false), m_bClosed(false)
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
void CReadWriteGate::Drain()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_condition.wait(lock,
        [this]()
        {
            return IsIdleLocked();
        });
}

// #endregion

// #region Submit

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

    // ① 拒绝路径：门已关闭 / 线程池不可用 → 未接受（调用方按「执行器不可用」收口）。
    if (m_bClosed.load() || !m_pPool->IsRunning())
    {
        return false;
    }

    // ② 统一入队（严格按提交顺序），随后在锁内尝试从队首放行。
    std::vector<CDispatchEntry> vecDispatch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        CDispatchEntry entry;
        entry.eKind = eKind;
        entry.fnTask = std::move(fnTask);
        m_dequeTasks.push_back(std::move(entry));
        PumpLocked(vecDispatch);
    }

    // ③ 已取得槽位的任务在锁外投递（投递动作不得持门锁：池的任务回调会回来拿它）。
    DispatchToPool(vecDispatch);
    return true;
}

// #endregion

// #region Internal

//================ Internal ================

/// @brief 空闲判定（持锁调用）。
///
/// @return true 无活跃任务、无排队任务。
bool CReadWriteGate::IsIdleLocked() const
{
    return m_nActiveReaders.load() == 0 && !m_bWriterActive.load() && m_dequeTasks.empty();
}

/// @brief 公平 FIFO：从队首顺序放行可准入的任务（持锁调用）。
///
/// 放行规则：
///  - 队首为读：无写者活跃且读槽位未满 → 放行；继续看下一个（读可并发）；
///  - 队首为写：无活跃读者 → 放行（独占）；否则停住 —— 其后的读/写一律等待，
///    既保证写不越过先前提交的任务，也保证写不被其后提交的读插队。
///
/// @param vecDispatch 输出：本轮取得槽位的任务（调用方锁外投递，可能一次放行多个读）。
void CReadWriteGate::PumpLocked(std::vector<CDispatchEntry>& vecDispatch)
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
            if (m_nMaxReaders > 0 && m_nActiveReaders.load() >= static_cast<int>(m_nMaxReaders))
            {
                break;  // 读槽位已满。
            }
            // 先移动到局部再出队，避免移动已弹出的元素。
            CDispatchEntry entry = std::move(m_dequeTasks.front());
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
            CDispatchEntry entry = std::move(m_dequeTasks.front());
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
/// 唤醒 `Drain()` 的等待方；新放行的任务在锁外投递。
///
/// @param eKind 刚结束任务的类别（决定归还哪个槽位）。
void CReadWriteGate::OnTaskExit(TaskKind eKind)
{
    std::vector<CDispatchEntry> vecDispatch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // ① 归还槽位。
        if (eKind == TaskKind::kRead)
        {
            m_nActiveReaders.fetch_sub(1);
        }
        else
        {
            m_bWriterActive.store(false);
        }

        // ② 槽位空出 → 按序放行后续任务（可能一次放行多个读）。
        PumpLocked(vecDispatch);
        m_condition.notify_all();
    }

    // ③ 锁外投递（与 Submit 同一约定：投递动作不持门锁）。
    DispatchToPool(vecDispatch);
}

/// @brief 把已取得槽位的任务包装并投递到线程池（锁外调用）。
///
/// 包装做两件事：跑任务体、**无论成败都归还槽位**。任务体抛出的异常在这里兜住并报告诊断 ——
/// 线程池的工作线程不捕获异常（异常逃出线程函数即 `std::terminate`），而在门这一层遗漏
/// 归还槽位会让模块永久卡死（写者标志再也回不来）。
///
/// 投递失败（线程池刚被停）时回滚槽位并把条目按原顺序放回队首 —— 不丢序、不漏槽位；
/// 此时任务留在队列里，由调用方按「Close → Drain → 停池」的顺序保证它跑完。
///
/// @param vecDispatch 本轮取得槽位的任务（处理完会被清空）。
void CReadWriteGate::DispatchToPool(std::vector<CDispatchEntry>& vecDispatch)
{
    std::vector<CDispatchEntry> vecFailed;
    for (size_t i = 0; i < vecDispatch.size(); ++i)
    {
        const TaskKind eKind = vecDispatch[i].eKind;
        std::function<void()> fnTask = std::move(vecDispatch[i].fnTask);

        // 包装：执行 + 归还槽位；异常不逃出线程池线程（报告诊断后照常归还）。
        std::function<void()> fnWrapped = [this, eKind, fnTask]()
        {
            try
            {
                fnTask();
            }
            catch (...)
            {
                ReportDiagnostic(detail::kDiagGateThrow);
            }
            OnTaskExit(eKind);
        };
        if (m_pPool->Submit(std::move(fnWrapped)))
        {
            continue;
        }

        // 线程池不可用：稍后回滚槽位并放回队首（先收集，锁外统一的回滚临界区）。
        CDispatchEntry entry;
        entry.eKind = eKind;
        entry.fnTask = std::move(fnTask);
        vecFailed.push_back(std::move(entry));
    }

    if (!vecFailed.empty())
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (size_t i = 0; i < vecFailed.size(); ++i)
        {
            if (vecFailed[i].eKind == TaskKind::kRead)
            {
                m_nActiveReaders.fetch_sub(1);
            }
            else
            {
                m_bWriterActive.store(false);
            }
        }
        // 逆序 push_front，恢复与队首一致的原始顺序。
        for (std::vector<CDispatchEntry>::reverse_iterator it = vecFailed.rbegin(); it != vecFailed.rend(); ++it)
        {
            m_dequeTasks.push_front(std::move(*it));
        }
        m_condition.notify_all();
    }
    vecDispatch.clear();
}

// #endregion

// #region Query

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
int CReadWriteGate::ActiveReaders() const
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

// #endregion

}  // namespace async
}  // namespace common
