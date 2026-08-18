#include "Thread/thread_pool.h"

namespace common {

/// @brief 创建线程池。
///
/// @param threadCount 工作线程数量。
CThreadPool::CThreadPool(size_t threadCount)
    : m_nThreadCount(threadCount), m_bRunning(false), m_bStopping(false)
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

/// @brief 提交任务。
///
/// 将任务加入队列并唤醒一个空闲工作线程。
bool CThreadPool::Submit(const CTask& task)
{
    if (!task)
    {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_bRunning || m_bStopping)
        {
            return false;
        }
        m_dequeTasks.push_back(task);
    }
    m_condition.notify_one();
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

/// @brief 工作线程循环。
void CThreadPool::WorkerLoop()
{
    while (true)
    {
        CTask task;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_condition.wait(lock, [this]() { return m_bStopping || !m_dequeTasks.empty(); });
            if (m_bStopping && m_dequeTasks.empty())
            {
                break;
            }
            task = m_dequeTasks.front();
            m_dequeTasks.pop_front();
        }
        if (task)
        {
            task();
        }
    }
}

} // namespace common
