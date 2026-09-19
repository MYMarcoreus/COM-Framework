#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

#include "Async/Diagnostics.h"
#include "Thread/ThreadPool.h"

// ====================================================================
// 读写门（模块内「读并发 / 写独占」的准入闸）
//
// 职责（只有三件）：
//   ① 按类别把任务排进一条统一 FIFO 队列（严格按提交顺序）；
//   ② 从队首顺序放行 —— 读任务可多个并发进入，写任务独占（等先前读者排空，
//      其后的读/写一并等待，不越过该写）；
//   ③ 槽位释放后继续放行（非阻塞：进不了就留在队列，线程立即归还线程池）。
//
// 为什么需要它（「模块线程安全」在异步里怎么做）：
//   一个模块由一个执行器调度（见 AsyncExecutor.h），执行器一旦有多个工作线程，模块状态就会
//   同时被多个任务访问。在异步流程里「给数据加锁」解决不了问题 —— 锁没法跨挂起点持有
//   （await 前必须释放），而「每个回调各自加锁」等于把调度交给运气：拿不到锁的任务会占住
//   工作线程（线程池可能被拖垮），也没有公平性可言。读写门把「谁能进入模块」上移成调度层的
//   一件事：
//     - 读任务（kRead，约定无副作用）之间可并发 —— 只读访问彼此不冲突；
//     - 写任务（kWrite）独占 —— 排斥本门内所有读写任务，写者之间也串行。
//   于是模块数据在「读任务只读、写任务写完」的规约下不再需要自己的锁，等待也不占用任何线程。
//
// 公平性（为什么是「单一队列 + 队首放行」而不是读/写双队列）：
//   双队列 + 写优先会让排队中的写插到先前排队的读前面（同一条流程「先读后写」实际执行成
//   「写→读」）；统一队列严格按提交顺序放行，队首是写且读者未排空时直接停住，其后的读/写
//   一律等待 —— 后来的任务永远不会越过先前的任务（ServerCore/Exec 时代的同款实现即因此定型）。
//
// 就地（不投递、直接在当前线程继续）：
//   执行器侧的层派发有一条快路径 —— 本线程已经在本执行器的线程上、且正跑着本门的「同类」任务时，
//   下一层直接在当前线程接着跑（省一次入队 + 唤醒）。判定收敛在 CanRunInline 一处，只有两个条件：
//     - 同类：槽位已经在手（读任务里的读层 / 写任务里的写层），不需要也不允许换类别 ——
//             读任务里出现写层只能排队等别的读者退出，写任务里出现读层也只能排队；
//     - 无人在排队：有人排队就让路（就地会插队，破坏公平）。
//   注意「就地」不额外占槽位、也不额外归还：它跑在外层任务已经持有的槽位里。
//
// 与执行器的关系：由 CAsyncExecutor 组合（挂在执行器句柄上，与线程池同寿命），执行器的每条
//   投递路径都带类别过这道门；门本身不认识 promise / 协程 / 上下文，只认识「任务」。
//
// 线程安全：Submit / Close / Drain / 查询均可跨线程调用；队列与槽位计数在同一把锁内维护。
//
// 停止顺序（调用方契约）：先 Close()（拒新投递）→ 再 Drain()（等已接受的跑完）→ 最后停线程池。
//   反了会让队列里的任务永远投不出去（它们已经算「已接受」）。
// ====================================================================

namespace common {
namespace async {

/// @brief 任务类别：读任务可并发进入，写任务独占进入。
enum class TaskKind
{
    kRead,  ///< 读任务：可多个线程并发执行（约定：不修改模块状态）。
    kWrite  ///< 写任务：独占执行（排斥本门内所有读写任务）。
};

class CReadWriteGate;  // 前置声明（任务帧只存指针）。

namespace detail {

/// @brief 读写门侧诊断文案（集中一处：测试断言常量，而不是去匹配子串）。
constexpr const char* kDiagGateThrow = "读写门：任务抛出了异常（已兜住，未终止进程）";

/// @brief 「当前任务」帧：本线程正跑着哪个门的哪类任务（「就地」判定的唯一依据）。
///
/// 只有「过门投递出去的任务」会压帧（见 ReadWriteGate.cpp 的包装任务）——
/// 就地跑下来的层沿用外层任务的帧：同类，槽位已经在手，不需要新的占用。
/// 因此 `CanRunInline(kind)` 只需回答两件事：① 本线程手上是不是本门的同类槽位；② 有没有人在排队。
struct CTaskFrame
{
    const CReadWriteGate* pGate;  ///< 所属门。
    TaskKind eKind;               ///< 任务类别。
    const CTaskFrame* pPrev;      ///< 外层帧（弹栈还原用；不在任务里时为 nullptr）。
};

/// @brief 当前线程的任务帧栈顶（不在任何任务里 = nullptr）。
///
/// @return 栈顶帧的引用（可读、可写：守卫在构造 / 析构时改写它）。
inline const CTaskFrame*& TaskFrameTop()
{
    static thread_local const CTaskFrame* s_pFrameTop = nullptr;
    return s_pFrameTop;
}

/// @brief 任务帧守卫（构造压栈、析构弹栈）。
///
/// 压帧的时机 = 任务体开跑之前；弹帧 = 任务体跑完（异常路径也弹，因为它是栈上对象）。
struct CTaskFrameGuard
{
    CTaskFrame m_frame;  ///< 本帧（地址稳定：守卫活在任务的调用栈上）。

    CTaskFrameGuard(const CReadWriteGate* pGate, TaskKind eKind) : m_frame{pGate, eKind, TaskFrameTop()}
    {
        TaskFrameTop() = &m_frame;
    }

    ~CTaskFrameGuard()
    {
        TaskFrameTop() = m_frame.pPrev;
    }

    CTaskFrameGuard(const CTaskFrameGuard&) = delete;
    CTaskFrameGuard& operator=(const CTaskFrameGuard&) = delete;
};

}  // namespace detail

/// @brief 模块内读写准入闸（读并发 / 写独占 / 公平 FIFO）。
///
/// 典型用法：
///   gate.Submit(TaskKind::kRead, fnRead);    // 只读访问：可与其它读并发
///   gate.Submit(TaskKind::kWrite, fnWrite);  // 修改模块状态：独占
///
/// @note 门不关心任务跑在哪条线程上（那是线程池的事），也不认识上下文与链 ——
///       它只回答一个问题：「这个任务现在能不能进入模块」，进不了就排队。
class CReadWriteGate
{
public:
    //================ Lifecycle ================

    // 创建读写门（绑定线程池：池只执行、门只调度）。
    explicit CReadWriteGate(common::thread::CThreadPool* pPool, size_t nMaxReaders = 0);

    // 不可拷贝（含互斥量与条件变量；拷贝会共享队列与槽位计数）。
    CReadWriteGate(const CReadWriteGate&) = delete;
    CReadWriteGate& operator=(const CReadWriteGate&) = delete;

    // 关闭本门：拒绝新提交（已入队 / 已在跑的任务不受影响）。
    void Close();

    // 等待排空（无活跃、无排队才返回；建议先 Close 再 Drain）。
    void Drain();

    //================ Submit ================

    // 提交任务（线程安全；严格按提交顺序入队，能放行则立即投递到线程池）。
    bool Submit(TaskKind eKind, std::function<void()> fnTask);

    //================ Inline ================

    // 能否「就地」跑本层（不投递、直接在当前线程继续）。
    bool CanRunInline(TaskKind eKind) const;

    //================ Query ================

    // 是否已关闭。
    bool IsClosed() const;

    // 是否空闲（无活跃、无排队）。
    bool IsIdle() const;

    // 排队中的任务数（持锁统计）。
    size_t PendingCount() const;

    // 当前活跃（已放行、未结束）的读任务数。
    int ActiveReaders() const;

    // 当前是否有写任务在跑。
    bool HasActiveWriter() const;

private:
    //================ Internal ================

    /// @brief 待调度条目：任务类别 + 任务体（统一 FIFO 队列）。
    struct CDispatchEntry
    {
        TaskKind eKind;                ///< 任务类别。
        std::function<void()> fnTask;  ///< 任务体。
    };

    /// @brief 过门任务包装（把「跑任务 + 归还槽位」合成一个「可移动」目标）。
    ///
    /// 为什么不用 lambda 包一层：C++11 的 lambda 不能「移动捕获」——包一层会把任务体
    /// 拷一份，而 `std::function` 的拷贝要再走一次堆分配（每次投递 +2 次分配）。
    /// 本类型可移动：构造时把任务体「移」进来，之后全程移动存储 ——
    /// 于是每次投递只花一次分配（`std::function` 存它自己的目标块），符合门定下的开销预算。
    struct CWrappedTask
    {
        CReadWriteGate* pGate;         ///< 所属门（归还槽位用）。
        TaskKind eKind;                ///< 任务类别。
        std::function<void()> fnTask;  ///< 任务体。

        /// @brief 构造（移动任务体）。
        CWrappedTask(CReadWriteGate* pGate, TaskKind eKind, std::function<void()> fnTask)
            : pGate(pGate), eKind(eKind), fnTask(std::move(fnTask))
        {}

        /// @brief 移动构造（`std::function` 存放目标时用它，避免拷贝任务体）。
        CWrappedTask(CWrappedTask&& other) : pGate(other.pGate), eKind(other.eKind), fnTask(std::move(other.fnTask))
        {}

        /// @brief 拷贝构造（只为满足 `std::function` 目标的 CopyConstructible 要求）。
        ///
        /// @note 实际不会走到：投递全程是移动（`Submit(CTask&&)` → 池队列移动存储）。
        CWrappedTask(const CWrappedTask& other) : pGate(other.pGate), eKind(other.eKind), fnTask(other.fnTask)
        {}

        CWrappedTask& operator=(const CWrappedTask&) = delete;
        CWrappedTask& operator=(CWrappedTask&&) = delete;

        /// @brief 执行任务体（压「当前任务」帧 → 兜异常 → 归还槽位）。
        void operator()()
        {
            const detail::CTaskFrameGuard frame(pGate, eKind);  // 跑完自动弹帧（异常路径也弹）。
            try
            {
                fnTask();
            }
            catch (...)
            {
                ReportDiagnostic(detail::kDiagGateThrow);
            }
            pGate->OnTaskExit(eKind);
        }
    };

    // 空闲判定（持锁调用）。
    bool IsIdleLocked() const;

    // 放行收集缓冲（线程局部复用，省掉每次投递的临时 vector 分配）。
    static std::vector<CDispatchEntry>& ScratchDispatchBuffer();

    // 公平 FIFO：从队首顺序放行可准入的任务（持锁调用）。
    void PumpLocked(std::vector<CDispatchEntry>& vecDispatch);

    // 任务结束：归还槽位并继续泵出（在线程池线程中调用）。
    void OnTaskExit(TaskKind eKind);

    // 把已取得槽位的任务包装并投递到线程池（锁外调用）。
    void DispatchToPool(std::vector<CDispatchEntry>& vecDispatch);

    common::thread::CThreadPool* m_pPool;  ///< 线程池（只执行；生命周期由调用方保证比门长）。
    size_t m_nMaxReaders;                  ///< 最大并发读任务数（0 = 不设上限）。

    std::deque<CDispatchEntry> m_dequeTasks;  ///< 统一 FIFO 队列（队首最先准入）。
    std::atomic<int> m_nActiveReaders;        ///< 当前活跃读任务数。
    std::atomic<bool> m_bWriterActive;        ///< 当前是否有写任务在跑。
    std::atomic<bool> m_bClosed;              ///< 是否已关闭（拒绝新提交）。

    mutable std::mutex m_mutex;           ///< 保护队列与槽位计数。
    std::condition_variable m_condition;  ///< 供 Drain() 等待排空。
};

}  // namespace async
}  // namespace common
