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
      m_nActiveReaders(0), m_bWriterActive(false), m_bWriterWaiting(false)
{
}

CModuleScheduler::~CModuleScheduler()
{
}

/// @brief 提交读/写子任务（线程安全）。
bool CModuleScheduler::Submit(ETaskKind eKind, const std::function<void()>& fnTask)
{
    if (m_pPool == nullptr || !m_pPool->IsRunning())
    {
        return false;
    }

    std::vector<CDispatchEntry> vecDispatch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (CanEnterLocked(eKind))
        {
            // 立即进入：占槽位
            if (eKind == ETaskKind::kRead)
            {
                m_nActiveReaders.fetch_add(1);
            }
            else
            {
                m_bWriterActive.store(true);
            }
            CDispatchEntry entry;
            entry.eKind = eKind;
            entry.fnTask = fnTask;
            vecDispatch.push_back(entry);
        }
        else if (eKind == ETaskKind::kRead)
        {
            m_dequeReadQueue.push_back(fnTask);
        }
        else
        {
            m_dequeWriteQueue.push_back(fnTask);
            m_bWriterWaiting = true; // 有写者在等 → 阻止新读者（防写饥饿）
        }
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

/// @brief 判断某类任务当前能否进入（持有锁时调用）。
bool CModuleScheduler::CanEnterLocked(ETaskKind eKind) const
{
    if (m_bWriterActive.load())
    {
        return false; // 写者独占中，一切任务不得进入
    }
    if (eKind == ETaskKind::kWrite)
    {
        return m_nActiveReaders.load() == 0; // 写者进入条件：无活跃读者
    }
    if (m_bWriterWaiting)
    {
        return false; // 有写者等待 → 阻止新读者（防写饥饿）
    }
    if (m_nMaxReaders > 0 && m_nActiveReaders.load() >= static_cast<int>(m_nMaxReaders))
    {
        return false; // 达到最大并发读上限
    }
    return true;
}

/// @brief 从队列取出可执行子任务（持有锁时调用）。
void CModuleScheduler::PumpLocked(std::vector<CDispatchEntry>& vecDispatch)
{
    // 有写者等待且当前无写者/读者 → 优先放行一个写者
    if (!m_bWriterActive.load() && m_nActiveReaders.load() == 0 && !m_dequeWriteQueue.empty())
    {
        CDispatchEntry entry;
        entry.eKind = ETaskKind::kWrite;
        entry.fnTask = std::move(m_dequeWriteQueue.front());
        m_dequeWriteQueue.pop_front();
        m_bWriterWaiting = !m_dequeWriteQueue.empty();
        m_bWriterActive.store(true);
        vecDispatch.push_back(std::move(entry));
        return; // 写者独占，放行后不再继续
    }

    // 放行读者（无写者活跃/等待，未超上限）
    while (!m_bWriterActive.load() && !m_bWriterWaiting &&
           (m_nMaxReaders == 0 || m_nActiveReaders.load() < static_cast<int>(m_nMaxReaders)) &&
           !m_dequeReadQueue.empty())
    {
        CDispatchEntry entry;
        entry.eKind = ETaskKind::kRead;
        entry.fnTask = std::move(m_dequeReadQueue.front());
        m_dequeReadQueue.pop_front();
        m_nActiveReaders.fetch_add(1);
        vecDispatch.push_back(std::move(entry));
    }
}

/// @brief 把已获取槽位的子任务包装并投递到线程池（锁外调用）。
void CModuleScheduler::DispatchToPool(std::vector<CDispatchEntry>& vecDispatch)
{
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
        if (!m_pPool->Submit(std::move(fnWrapped)))
        {
            // 线程池不可用：归还槽位并重新入队，防止槽位泄漏。
            std::lock_guard<std::mutex> lock(m_mutex);
            if (eKind == ETaskKind::kRead)
            {
                m_nActiveReaders.fetch_sub(1);
                m_dequeReadQueue.push_front(fnTask);
            }
            else
            {
                m_bWriterActive.store(false);
                m_dequeWriteQueue.push_front(fnTask);
                m_bWriterWaiting = true;
            }
            m_condition.notify_all();
        }
    }
    vecDispatch.clear();
}

/// @brief 排队中的子任务数（持锁统计）。
size_t CModuleScheduler::PendingCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_dequeReadQueue.size() + m_dequeWriteQueue.size();
}

/// @brief 是否空闲（无活跃、无排队）。
bool CModuleScheduler::IsIdle() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_nActiveReaders.load() == 0 && !m_bWriterActive.load() &&
           m_dequeReadQueue.empty() && m_dequeWriteQueue.empty();
}

/// @brief 等待排空（Stop/Shutdown 时由编排线程调用；不阻止新提交）。
void CModuleScheduler::Drain()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_condition.wait(lock, [this]()
    {
        return m_nActiveReaders.load() == 0 && !m_bWriterActive.load() &&
               m_dequeReadQueue.empty() && m_dequeWriteQueue.empty();
    });
}

} // namespace sc
