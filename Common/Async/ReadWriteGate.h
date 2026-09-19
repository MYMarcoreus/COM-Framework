#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

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

namespace detail {

/// @brief 读写门侧诊断文案（集中一处：测试断言常量，而不是去匹配子串）。
constexpr const char* kDiagGateThrow = "读写门：任务抛出了异常（已兜住，未终止进程）";

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

    // 空闲判定（持锁调用）。
    bool IsIdleLocked() const;

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
