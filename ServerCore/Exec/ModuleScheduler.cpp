#include "Exec/ModuleScheduler.h"

#include <exception>
#include <string>
#include <utility>

#include "Log/Logger.h"

namespace sc {

namespace {

/// @brief 记录当前异常（仅在 catch 块中调用；线程池 worker 不捕获异常，
///        任务抛异常会导致线程终止，故框架必须捕获并记录）。
void LogCaughtException(const char* szWhere)
{
    try
    {
        throw;
    }
    catch (const std::exception& e)
    {
        common::log::CLogger::Instance().Error(
            std::string(szWhere) + " 子任务异常: " + e.what());
    }
    catch (...)
    {
        common::log::CLogger::Instance().Error(std::string(szWhere) + " 子任务未知异常");
    }
}

} // namespace

/// @brief 创建模块级调度器。
///
/// @param pPool 全局线程池（仅执行不调度；生命周期由调用方管理）。
/// @param nMaxReaders 最大并发读线程数（0 = 不设上限）。
CModuleScheduler::CModuleScheduler(common::thread::CThreadPool* pPool, size_t nMaxReaders)
    : m_pPool(pPool), m_nMaxReaders(nMaxReaders),
      m_nActiveReaders(0), m_bWriterActive(false)
{
}

CModuleScheduler::~CModuleScheduler()
{
}

/// @brief 提交读/写子任务（线程安全，严格按提交顺序入队）。
///
/// 统一入队后立即尝试从队首放行；无法进入的子任务留在队列，槽位释放后按序执行。
bool CModuleScheduler::Submit(ETaskKind eKind, const std::function<void()>& fnTask)
{
    if (m_pPool == nullptr || !m_pPool->IsRunning())
    {
        return false;
    }

    std::vector<CDispatchEntry> vecDispatch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        CDispatchEntry entry;
        entry.eKind = eKind;
        entry.fnTask = fnTask;
        m_dequeTasks.push_back(entry); // 统一入队（严格按提交顺序）。
        PumpLocked(vecDispatch);       // 队首可准入则放行。
    }
    DispatchToPool(vecDispatch);
    return true;
}

/// @brief 子任务执行结束：归还槽位并继续泵出（在线程池线程中调用）。
void CModuleScheduler::OnTaskExit(ETaskKind eKind)
{
    std::vector<CDispatchEntry> vecDispatch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (eKind == ETaskKind::kRead)
        {
            m_nActiveReaders.fetch_sub(1);
        }
        else
        {
            m_bWriterActive.store(false);
        }
        PumpLocked(vecDispatch);
        m_condition.notify_all(); // 唤醒 Drain()
    }
    DispatchToPool(vecDispatch);
}

/// @brief 公平 FIFO：从队首顺序放行可执行子任务（持有锁时调用）。
///
///  - 队首为读：无写者活跃且读槽位未满 → 放行；继续看下一个（读可并发）；
///  - 队首为写：无活跃读者 → 放行（独占）；否则阻塞（其后的读/写一律等待，
///    保证写不会越过先前提交的任务，也不被其后提交的读插队）。
void CModuleScheduler::PumpLocked(std::vector<CDispatchEntry>& vecDispatch)
{
    while (!m_dequeTasks.empty())
    {
        // 仅读取队首类型做准入判定（不持有引用：pop_front 会使引用悬垂）。
        const ETaskKind eFront = m_dequeTasks.front().eKind;
        if (eFront == ETaskKind::kRead)
        {
            if (m_bWriterActive.load())
            {
                break; // 写者独占中。
            }
            if (m_nMaxReaders > 0 &&
                m_nActiveReaders.load() >= static_cast<int>(m_nMaxReaders))
            {
                break; // 读槽位已满。
            }
            // 先移出再 pop，避免移动已销毁元素。
            CDispatchEntry entry = std::move(m_dequeTasks.front());
            m_dequeTasks.pop_front();
            m_nActiveReaders.fetch_add(1);
            vecDispatch.push_back(std::move(entry));
        }
        else // kWrite
        {
            if (m_bWriterActive.load())
            {
                break;
            }
            if (m_nActiveReaders.load() > 0)
            {
                break; // 等先前读者排空（其后的任务一并等待，不越过该写）。
            }
            CDispatchEntry entry = std::move(m_dequeTasks.front());
            m_dequeTasks.pop_front();
            m_bWriterActive.store(true);
            vecDispatch.push_back(std::move(entry));
            break; // 写者独占，其后任务待写完成。
        }
    }
}

/// @brief 把已获取槽位的子任务包装并投递到线程池（锁外调用）。
void CModuleScheduler::DispatchToPool(std::vector<CDispatchEntry>& vecDispatch)
{
    std::vector<CDispatchEntry> vecFailed;
    for (std::vector<CDispatchEntry>::iterator it = vecDispatch.begin();
         it != vecDispatch.end(); ++it)
    {
        ETaskKind eKind = it->eKind;
        std::function<void()> fnTask = std::move(it->fnTask);
        // 包装：执行子任务 + 归还槽位并继续泵出；异常捕获防线程终止。
        std::function<void()> fnWrapped = [this, eKind, fnTask]()
        {
            try
            {
                fnTask();
            }
            catch (...)
            {
                LogCaughtException("CModuleScheduler");
            }
            OnTaskExit(eKind);
        };
        if (m_pPool->Submit(std::move(fnWrapped)))
        {
            continue;
        }
        // 线程池不可用：记录，稍后按原顺序放回队首，防止槽位泄漏。
        CDispatchEntry entry;
        entry.eKind = eKind;
        entry.fnTask = fnTask;
        vecFailed.push_back(entry);
    }

    if (!vecFailed.empty())
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (std::vector<CDispatchEntry>::iterator it = vecFailed.begin();
             it != vecFailed.end(); ++it)
        {
            if (it->eKind == ETaskKind::kRead)
            {
                m_nActiveReaders.fetch_sub(1);
            }
            else
            {
                m_bWriterActive.store(false);
            }
        }
        // 逆序 push_front，保持与队首一致的原始顺序。
        for (std::vector<CDispatchEntry>::reverse_iterator it = vecFailed.rbegin();
             it != vecFailed.rend(); ++it)
        {
            m_dequeTasks.push_front(std::move(*it));
        }
        m_condition.notify_all();
    }
    vecDispatch.clear();
}

/// @brief 排队中的子任务数（持锁统计）。
size_t CModuleScheduler::PendingCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_dequeTasks.size();
}

/// @brief 是否空闲（无活跃、无排队）。
bool CModuleScheduler::IsIdle() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_nActiveReaders.load() == 0 && !m_bWriterActive.load() &&
           m_dequeTasks.empty();
}

/// @brief 等待排空（Stop/Shutdown 时由编排线程调用；不阻止新提交）。
void CModuleScheduler::Drain()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_condition.wait(lock, [this]()
    {
        return m_nActiveReaders.load() == 0 && !m_bWriterActive.load() &&
               m_dequeTasks.empty();
    });
}

} // namespace sc
