#include "Thread/ThreadPool.h"

#include <chrono>
#include <string>

#if defined(__linux__)
    #include <pthread.h>  // pthread_setname_np：给工作线程起名（调试用）。
#endif

namespace common {
namespace thread {

namespace {

/// @brief 当前线程所属的线程池（仅工作线程内有效，其它线程为 nullptr）。
thread_local const CThreadPool* tl_pCurrentPool = nullptr;

/// @brief 当前线程所属线程池的名字（共享所有权副本；仅工作线程内非空）。
///
/// 存 `shared_ptr` 而不是裸指针：上层（异步 trace）会把它记到「某一层」上，
/// 那一层可能比线程池活得久 —— 共享所有权副本既零分配（只加引用计数）又不会悬垂。
thread_local std::shared_ptr<const std::string> tl_spCurrentPoolName;

/// @brief 工作线程作用域标记（进入设、退出清）——线程亲和的判定基础。
struct CWorkerPoolScope
{
    CWorkerPoolScope(const CThreadPool* pPool, const std::shared_ptr<const std::string>& spName)
    {
        tl_pCurrentPool = pPool;
        tl_spCurrentPoolName = spName;
    }

    ~CWorkerPoolScope()
    {
        tl_pCurrentPool = nullptr;
        tl_spCurrentPoolName.reset();
    }
};

/// @brief 给当前线程起个名字（供 gdb `info threads` / htop / top -H 里辨认）。
///
/// 名字取 `<池名>-<序号>`；Linux 的线程名上限是 15 字节（含结尾 `\0`），超长截断。
/// 池名为空（或非 Linux 平台）时什么都不做 —— 线程名只是调试便利，不影响任何逻辑。
///
/// @param strPoolName 池名（空 = 不起名）。
/// @param nIndex 本 worker 的序号（从 0 起）。
void SetWorkerThreadName(const std::string& strPoolName, size_t nIndex)
{
#if defined(__linux__)
    if (strPoolName.empty())
    {
        return;
    }
    std::string strName = strPoolName + "-" + std::to_string(nIndex);
    if (strName.size() > 15)
    {
        strName.resize(15);
    }
    pthread_setname_np(pthread_self(), strName.c_str());
#else
    (void)strPoolName;
    (void)nIndex;
#endif
}

}  // namespace

/// @brief 创建线程池。
///
/// @param nThreadCount 工作线程数量。
/// @param strName 池名（只用于给工作线程起名 / 日志辨认；空 = 不起名）。
CThreadPool::CThreadPool(size_t nThreadCount, const std::string& strName)
    : m_nPending(0),
      m_nIdleWorkers(0),
      m_nThreadCount(nThreadCount),
      m_spName(strName.empty() ? std::shared_ptr<const std::string>() : std::make_shared<const std::string>(strName)),
      m_bRunning(false),
      m_bStopping(false)
{}

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
    m_nIdleWorkers = 0;  // 线程尚未投入，空闲计数清零。
    for (size_t i = 0; i < m_nThreadCount; ++i)
    {
        m_vecWorkers.push_back(std::thread(&CThreadPool::WorkerLoop, this, i));
    }
    return true;
}

/// @brief 提交任务（拷贝投递）。
///
/// 将任务加入队列；队列空→非空唤醒 1 个线程（流式单任务场景），
/// 排队出现积压时按空闲线程数补唤醒，保证突发批量任务的并行度。
bool CThreadPool::Submit(const CTask& fnTask)
{
    if (!fnTask)
    {
        return false;
    }
    bool bNeedNotify = false;
    size_t nExtra = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_bRunning || m_bStopping)
        {
            return false;
        }
        bNeedNotify = m_dequeTasks.empty();  // 队列空→非空：至少唤醒 1 个。
        m_dequeTasks.push_back(fnTask);
        m_nPending.fetch_add(1, std::memory_order_relaxed);
        // 突发积压：排队任务数 > 1 时按空闲线程补唤醒，提升并行度；
        // 流式单任务排队≈1，不补唤醒（保持单线程顺流处理，避免无谓唤醒）。
        const size_t nQueued = m_dequeTasks.size();
        if (nQueued > 1)
        {
            const size_t nBacklog = nQueued - 1;
            nExtra = nBacklog > m_nIdleWorkers ? m_nIdleWorkers : nBacklog;
        }
    }
    if (bNeedNotify)
    {
        m_condition.notify_one();
    }
    for (size_t i = 0; i < nExtra; ++i)
    {
        m_condition.notify_one();
    }
    return true;
}

/// @brief 提交任务（移动投递，避免 std::function 拷贝）。
///
/// 将任务移动进队列；唤醒策略与拷贝版一致。
bool CThreadPool::Submit(CTask&& fnTask)
{
    if (!fnTask)
    {
        return false;
    }
    bool bNeedNotify = false;
    size_t nExtra = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_bRunning || m_bStopping)
        {
            return false;
        }
        bNeedNotify = m_dequeTasks.empty();
        m_dequeTasks.push_back(std::move(fnTask));
        m_nPending.fetch_add(1, std::memory_order_relaxed);
        const size_t nQueued = m_dequeTasks.size();
        if (nQueued > 1)
        {
            const size_t nBacklog = nQueued - 1;
            nExtra = nBacklog > m_nIdleWorkers ? m_nIdleWorkers : nBacklog;
        }
    }
    if (bNeedNotify)
    {
        m_condition.notify_one();
    }
    for (size_t i = 0; i < nExtra; ++i)
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

/// @brief 返回待处理任务数（队列中未取出的；轻量原子读，不加锁）。
size_t CThreadPool::PendingCount() const
{
    return static_cast<size_t>(m_nPending.load(std::memory_order_relaxed));
}

/// @brief 当前线程是否本线程池的工作线程。
///
/// @param pPool 线程池指针（可为空）。
/// @return true 当前线程是本池工作线程。
bool CThreadPool::IsInPoolThread(const CThreadPool* pPool)
{
    return pPool != nullptr && tl_pCurrentPool == pPool;
}

/// @brief 当前线程所属线程池的名字。
std::shared_ptr<const std::string> CThreadPool::CurrentPoolName()
{
    return tl_spCurrentPoolName;
}

/// @brief 工作线程循环。
void CThreadPool::WorkerLoop(size_t nIndex)
{
    CWorkerPoolScope poolScope(this, m_spName);  // 标记本线程归属（线程亲和判定用）。
    SetWorkerThreadName(Name(), nIndex);         // 调试用：让这条线程在 gdb/htop 里可辨认。
    while (true)
    {
        CTask fnTask;
        bool bWasIdle = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_dequeTasks.empty() && !m_bStopping)
            {
                // 进入空闲（自旋/等待），供 Submit 按空闲线程数精确唤醒。
                ++m_nIdleWorkers;
                bWasIdle = true;
                // 混合等待：短暂自旋（读原子 pending，不持锁），任务刚提交时
                // 线程未睡可直接取，减少「睡→醒」futex 往返。
                lock.unlock();
                const auto spinUntil = std::chrono::steady_clock::now() + std::chrono::microseconds(30);
                while (std::chrono::steady_clock::now() < spinUntil)
                {
                    if (m_nPending.load(std::memory_order_acquire) > 0) break;  // 有任务入队 → 回锁直接取。
                    std::this_thread::yield();
                }
                lock.lock();
                if (m_dequeTasks.empty())
                {
                    m_condition.wait(lock,
                        [this]()
                        {
                            return m_bStopping || !m_dequeTasks.empty();
                        });
                }
            }
            if (m_bStopping && m_dequeTasks.empty())
            {
                if (bWasIdle)
                {
                    --m_nIdleWorkers;  // 退出空闲。
                }
                break;
            }
            fnTask = m_dequeTasks.front();
            m_dequeTasks.pop_front();
            m_nPending.fetch_sub(1, std::memory_order_relaxed);
            if (bWasIdle)
            {
                --m_nIdleWorkers;  // 空闲 → 忙碌。
            }
        }
        if (fnTask)
        {
            fnTask();
        }
    }
}

}  // namespace thread
}  // namespace common
