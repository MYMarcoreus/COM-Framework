#include "Thread/ThreadPool.h"

#include <chrono>

namespace common {
namespace thread {

/// @brief 创建线程池。
///
/// @param nThreadCount 工作线程数量。
CThreadPool::CThreadPool(size_t nThreadCount)
    : m_nPending(0), m_nThreadCount(nThreadCount),
      m_bRunning(false), m_bStopping(false)
{
}

/// @brief 销毁线程池。
CThreadPool::~CThreadPool()
{
    Stop();
}

/// @brief 启动工作线程。
bool CThreadPool::Start()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_bRunning)
    {
        return false;
    }
    if (m_nThreadCount == 0)
    {
        return false;
    }
    m_bRunning = true;
    m_bStopping = false;
    for (size_t i = 0; i < m_nThreadCount; ++i)
    {
        m_vecWorkers.push_back(std::thread(&CThreadPool::WorkerLoop, this));
    }
    return true;
}

/// @brief 提交任务（拷贝投递）。
///
/// 将任务加入队列并唤醒一个空闲工作线程。
bool CThreadPool::Submit(const CTask& fnTask)
{
    if (!fnTask)
    {
        return false;
    }
    bool bNeedNotify = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_bRunning || m_bStopping)
        {
            return false;
        }
        bNeedNotify = m_dequeTasks.empty(); // 队列空→非空才需唤醒（工作线程可能在睡）。
        m_dequeTasks.push_back(fnTask);
        m_nPending.fetch_add(1, std::memory_order_relaxed);
    }
    if (bNeedNotify)
    {
        m_condition.notify_one();
    }
    return true;
}

/// @brief 提交任务（移动投递，避免 std::function 拷贝）。
///
/// 将任务移动进队列并唤醒一个空闲工作线程。
bool CThreadPool::Submit(CTask&& fnTask)
{
    if (!fnTask)
    {
        return false;
    }
    bool bNeedNotify = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_bRunning || m_bStopping)
        {
            return false;
        }
        bNeedNotify = m_dequeTasks.empty(); // 队列空→非空才需唤醒。
        m_dequeTasks.push_back(std::move(fnTask));
        m_nPending.fetch_add(1, std::memory_order_relaxed);
    }
    if (bNeedNotify)
    {
        m_condition.notify_one();
    }
    return true;
}

/// @brief 停止线程池。
///
/// 通知所有工作线程退出并等待其结束。
void CThreadPool::Stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_bRunning)
        {
            return;
        }
        m_bStopping = true;
    }
    m_condition.notify_all();
    for (size_t i = 0; i < m_vecWorkers.size(); ++i)
    {
        if (m_vecWorkers[i].joinable())
        {
            m_vecWorkers[i].join();
        }
    }
    m_vecWorkers.clear();
    m_bRunning = false;
    m_bStopping = false;
}

/// @brief 返回工作线程数量。
size_t CThreadPool::ThreadCount() const
{
    return m_nThreadCount;
}

/// @brief 是否正在运行。
bool CThreadPool::IsRunning() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_bRunning;
}

/// @brief 返回待处理任务数（队列中未取出的）。
///
/// 轻量原子读（不加锁）；供协程负载感知判断（队列空 → 内联续接安全）。
size_t CThreadPool::PendingCount() const
{
    return static_cast<size_t>(m_nPending.load(std::memory_order_relaxed));
}

/// @brief 工作线程循环。
void CThreadPool::WorkerLoop()
{
    while (true)
    {
        CTask fnTask;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_dequeTasks.empty() && !m_bStopping)
            {
                // A1 混合等待：短暂自旋（读原子 pending，不持锁），任务刚提交时
                // 线程未睡可直接取，减少「睡→醒」futex 往返（单任务延迟主要成本）。
                lock.unlock();
                const auto spinUntil =
                    std::chrono::steady_clock::now() + std::chrono::microseconds(30);
                while (std::chrono::steady_clock::now() < spinUntil)
                {
                    if (m_nPending.load(std::memory_order_acquire) > 0)
                        break; // 有任务入队 → 回锁直接取。
                    std::this_thread::yield();
                }
                lock.lock();
            }
            m_condition.wait(lock, [this]() { return m_bStopping || !m_dequeTasks.empty(); });
            if (m_bStopping && m_dequeTasks.empty())
            {
                break;
            }
            fnTask = m_dequeTasks.front();
            m_dequeTasks.pop_front();
            m_nPending.fetch_sub(1, std::memory_order_relaxed);
        }
        if (fnTask)
        {
            fnTask();
        }
    }
}

} // namespace thread
} // namespace common
