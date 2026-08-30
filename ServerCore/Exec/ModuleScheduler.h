#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

#include "Thread/ThreadPool.h"

namespace sc {

/// @brief 模块级读写（子）任务调度器。
///
/// 控制进入本模块的读/写子任务并发度：
///  - 读任务（kRead）：可多个线程并发进入（上限可配，0 = 不设上限）；
///  - 写任务（kWrite）：独占，排斥本模块内所有读写子任务；
///  - 公平性：有写任务在等待时不放行新读任务（避免写饥饿）。
///
/// 非阻塞语义：进不了模块的子任务进入等待队列，槽位释放后由调度器
/// 重新投递到全局线程池执行；线程不被占用、及时归还。
///
/// 线程安全：任意线程可 Submit；槽位计数与等待队列在同一把锁内维护。
/// 依赖全局线程池（common::thread::CThreadPool）只执行、不调度。
class CModuleScheduler
{
public:
    /// @brief 子任务类型。
    enum class ETaskKind
    {
        kRead,  ///< 读任务：可多个线程并发进入
        kWrite  ///< 写任务：独占进入
    };

    /// @brief 构造。
    ///
    /// @param pPool 全局线程池（仅执行不调度；生命周期由调用方管理）。
    /// @param nMaxReaders 最大并发读线程数（0 = 不设上限，仅受线程池约束）。
    explicit CModuleScheduler(common::thread::CThreadPool* pPool, size_t nMaxReaders = 0);

    virtual ~CModuleScheduler();

    /// @brief 提交读/写子任务（线程安全）。
    ///
    /// 可立即进入则投递到线程池执行；否则排队，槽位释放后重投递。
    ///
    /// @param eKind 子任务类型。
    /// @param fnTask 子任务逻辑（不应抛异常；异常被捕获并记录日志）。
    /// @return true 已投递或已排队；false 线程池不可用。
    bool Submit(ETaskKind eKind, const std::function<void()>& fnTask);

    /// @brief 当前活跃读线程数（原子读，近似值，仅诊断用）。
    int ActiveReaders() const { return m_nActiveReaders.load(); }

    /// @brief 当前是否有写线程在执行（原子读，近似值，仅诊断用）。
    bool HasActiveWriter() const { return m_bWriterActive.load(); }

    /// @brief 排队中的子任务数（持锁统计）。
    size_t PendingCount() const;

    /// @brief 是否空闲（无活跃、无排队）。
    bool IsIdle() const;

    /// @brief 等待排空（Stop/Shutdown 时由编排线程调用；不阻止新提交）。
    void Drain();

private:
    /// @brief 待投递条目：子任务类型 + 子任务逻辑。
    struct CDispatchEntry
    {
        ETaskKind eKind;
        std::function<void()> fnTask;
    };

    // 判断某类任务当前能否进入（持有锁时调用）。
    bool CanEnterLocked(ETaskKind eKind) const;

    // 从队列取出可执行子任务（持有锁时调用，写入 vecDispatch）。
    void PumpLocked(std::vector<CDispatchEntry>& vecDispatch);

    // 子任务执行结束：归还槽位并继续泵出（在线程池线程中调用）。
    void OnTaskExit(ETaskKind eKind);

    // 把已获取槽位的子任务包装并投递到线程池（锁外调用）。
    void DispatchToPool(std::vector<CDispatchEntry>& vecDispatch);

    common::thread::CThreadPool* m_pPool;  // 全局线程池（生命周期由调用方管理）。
    size_t m_nMaxReaders;                   // 最大并发读线程数（0 = 不设上限）。

    std::deque<std::function<void()>> m_dequeReadQueue;  // 等待中的读任务。
    std::deque<std::function<void()>> m_dequeWriteQueue; // 等待中的写任务。
    std::atomic<int> m_nActiveReaders;    // 当前活跃读线程数。
    std::atomic<bool> m_bWriterActive;    // 当前是否有写线程在执行。
    bool m_bWriterWaiting;                // 是否有写任务在等待（防写饥饿，持锁访问）。

    mutable std::mutex m_mutex;
    std::condition_variable m_condition; // 供 Drain() 等待排空。
};

} // namespace sc
