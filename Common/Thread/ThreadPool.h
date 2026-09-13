#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace common {
namespace thread {

/// @brief 线程池。
///
/// 维护固定数量工作线程与任务队列，提交的任务由空闲线程执行。
/// 基于 C++11 标准库自实现，不依赖第三方库。
class CThreadPool
{
public:
    /// @brief 任务类型。
    using CTask = std::function<void()>;

    // 创建线程池（指定工作线程数与池名）。
    //
    // 池名只用于**调试**：启动时把每条 worker 线程命名为「<池名>-<序号>」（Linux 线程名上限
    // 15 字符，超长截断），于是 gdb 的 `info threads` / htop / top -H 里一眼能看出这条线程
    // 属于哪个池。传空串 = 不起名（保持系统默认）。
    explicit CThreadPool(size_t threadCount = 1, const std::string& strName = std::string());

    // 池名（空 = 未命名）。
    //
    // 返回引用：名字在构造时定下、之后不变（未命名时指向一个静态空串）。
    const std::string& Name() const
    {
        static const std::string s_strEmpty;
        return (m_spName != nullptr) ? *m_spName : s_strEmpty;
    }

    ~CThreadPool();

    // 启动工作线程。
    bool Start();

    // 提交任务（线程安全，拷贝投递）。
    bool Submit(const CTask& task);

    // 提交任务（线程安全，移动投递：避免 std::function 拷贝）。
    bool Submit(CTask&& task);

    // 停止线程池，等待所有已提交任务执行完毕。
    void Stop();

    // 工作线程数量。
    size_t ThreadCount() const;

    // 是否正在运行。
    bool IsRunning() const;

    // 待处理任务数（队列中未取出的）。
    size_t PendingCount() const;

    // 当前线程是否本线程池的工作线程。
    // 工作线程在 WorkerLoop 里给自己打 thread_local 标记；供上层做「线程亲和」判断：
    // 已经在本池线程上就地执行（省一次入队），否则投递回本池执行。
    static bool IsInPoolThread(const CThreadPool* pPool);

    // 当前线程所属线程池的名字（**不是**池工作线程 → 空）。
    //
    // 调试用：异步层可能「就地」跑在别的池的线程上，此时「正跑在哪个执行器上」只有线程自己
    // 知道 —— trace 就用它记下每层的执行器（拿它自己的 `shared_ptr`，零分配、不悬垂）。
    //
    // @return 池名（共享所有权副本；不是池线程 / 池未命名 → 空 shared_ptr）。
    static std::shared_ptr<const std::string> CurrentPoolName();

private:
    // 工作线程循环。
    //
    // @param nIndex 本 worker 的序号（从 0 起；用于给它起线程名）。
    void WorkerLoop(size_t nIndex);

    std::vector<std::thread> m_vecWorkers;
    std::deque<CTask> m_dequeTasks;
    std::atomic<long> m_nPending;  // 待处理任务数（Submit +1，WorkerLoop 取出 -1）。
    size_t m_nIdleWorkers;         // 空闲工作线程数（WorkerLoop 维护，Submit 用于按需唤醒）。
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    size_t m_nThreadCount;
    std::shared_ptr<const std::string> m_spName;  // 池名（只用于给 worker 线程起名 / 调试辨认）。
    bool m_bRunning;
    bool m_bStopping;
};

}  // namespace thread
}  // namespace common
