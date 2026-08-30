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
/// 控制进入本模块的读/写子任务并发度，并保证**公平 FIFO 执行顺序**：
///  - 读任务（kRead）：可多个线程并发进入（上限可配，0 = 不设上限）；
///  - 写任务（kWrite）：独占，排斥本模块内所有读写子任务；
///  - 公平 FIFO：统一队列严格按提交顺序放行——任务不会越过先前提交的任务执行；
///    排队的写不会插队到先前排队的读前面；队首写等待先前读者排空后再执行，
///    其后的读/写一并等待（不越过该写）。
///
/// 非阻塞语义：进不了模块的子任务留在队列中，槽位释放后由调度器重新投递
/// 到全局线程池执行；线程不被占用、及时归还。
///
/// 线程安全：任意线程可 Submit；队列与槽位计数在同一把锁内维护。
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

    /// @brief 提交读/写子任务（线程安全，严格按提交顺序入队）。
    ///
    /// 队首可准入则投递到线程池执行；否则留在队列，槽位释放后按序放行。
    ///
    /// @param eKind 子任务类型。
    /// @param fnTask 子任务逻辑（不应抛异常；异常被捕获并记录日志）。
    /// @return true 已入队（可能已投递）；false 线程池不可用。
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
    /// @brief 待调度条目：子任务类型 + 子任务逻辑（统一 FIFO 队列）。
    struct CDispatchEntry
    {
        ETaskKind eKind;
        std::function<void()> fnTask;
    };

    // 公平 FIFO：从队首顺序放行可执行子任务（持有锁时调用）。
    void PumpLocked(std::vector<CDispatchEntry>& vecDispatch);

    // 子任务执行结束：归还槽位并继续泵出（在线程池线程中调用）。
    void OnTaskExit(ETaskKind eKind);

    // 把已获取槽位的子任务包装并投递到线程池（锁外调用）。
    void DispatchToPool(std::vector<CDispatchEntry>& vecDispatch);

    common::thread::CThreadPool* m_pPool;  // 全局线程池（生命周期由调用方管理）。
    size_t m_nMaxReaders;                   // 最大并发读线程数（0 = 不设上限）。

    std::deque<CDispatchEntry> m_dequeTasks; // 统一 FIFO 队列（队首最先准入）。
    std::atomic<int> m_nActiveReaders;    // 当前活跃读线程数。
    std::atomic<bool> m_bWriterActive;    // 当前是否有写线程在执行。

    mutable std::mutex m_mutex;
    std::condition_variable m_condition; // 供 Drain() 等待排空。
};

} // namespace sc
