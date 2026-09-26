/// @file test_async_rw.cpp
/// 执行器 × 读写门（模块内「读并发 / 写独占」）的集成测试。
///
/// 正确性验证点：
///  - `Post(kWrite)`（写）与写链：与模块内其它任务互斥，写者唯一，读写不重叠；
///  - `Post(kRead)` 与读链（`NewPromise(..., TaskKind::kRead)`）：读之间可并发进入；
///  - 混合读写：读不越过写、写等到读者排空（无违例）；
///  - 逐层类别：同一链的层可读 / 写混排，每层按自己的类别过门；
///  - `kDirect`（直投）：不过门 —— 与在读 / 写任务并发，且不占槽位；
///  - 「等子链」层的工厂（`ThenPromise` / `ThenBridge`）：与层体一样按本层类别过门；
///  - 起链回调（`NewPromise(spCtx, fnStarter, 类别)`）：门外 / 别的门起链也按类别过门
///    （跨模块起链落在**被调模块自己**的门里），门内同类槽位则就地同步跑；
///  - 就地级联仍然保留：单线程执行器上一条链的各层跑在同一线程（无其它排队任务时）；
///  - 停止语义：`Stop()` 先关门的投递、再等已接受的任务跑完（不丢任务、不悬挂），
///    停止期间链的后续层以「执行器已停」收口。
///
/// 说明：断言只允许在主测试线程执行；工作线程仅更新原子状态。
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Async/AsyncExecutor.h"
#include "Async/GateGuard.h"
#include "Async/Promise.h"
#include "Async/ReadWriteGate.h"
#include "Coroutine/Coroutine.h"
#include "TestFramework.h"

namespace {

using common::async::CAsyncExecutor;
using common::async::CPromise;
using common::async::CPromiseResult;
using common::async::TaskKind;

// 读/写任务在模块内执行的持续时间（毫秒）：放大并发窗口便于观测。
const int kWorkMs = 2;

/// @brief 读写观测状态（工作线程只写原子，主线程等待后断言）。
struct SRwState
{
    SRwState() : nActiveReaders(0), nActiveWriters(0), nPeakReaders(0), nPeakWriters(0), nViolations(0)
    {}

    std::atomic<int> nActiveReaders;  // 当前读任务中的线程数
    std::atomic<int> nActiveWriters;  // 当前写任务中的线程数
    std::atomic<int> nPeakReaders;    // 观测到的最大并发读
    std::atomic<int> nPeakWriters;    // 观测到的最大并发写
    std::atomic<int> nViolations;     // 读写互斥违例次数（应为 0）
};

/// @brief 读业务：先校验无写者 → 并发计数并记录峰值 → 短耗时 → 退出。
void RunReadWork(SRwState* pState)
{
    if (pState->nActiveWriters.load() > 0)
    {
        pState->nViolations.fetch_add(1);
    }
    const int nNow = pState->nActiveReaders.fetch_add(1) + 1;
    int nPeak = pState->nPeakReaders.load();
    while (nPeak < nNow && !pState->nPeakReaders.compare_exchange_weak(nPeak, nNow))
    {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kWorkMs));
    pState->nActiveReaders.fetch_sub(1);
}

/// @brief 写业务：先校验无读者/无其他写者 → 独占计数 → 短耗时 → 退出。
void RunWriteWork(SRwState* pState)
{
    if (pState->nActiveReaders.load() > 0)
    {
        pState->nViolations.fetch_add(1);
    }
    const int nNow = pState->nActiveWriters.fetch_add(1) + 1;
    if (nNow > 1)
    {
        pState->nViolations.fetch_add(1);
    }
    int nPeak = pState->nPeakWriters.load();
    while (nPeak < nNow && !pState->nPeakWriters.compare_exchange_weak(nPeak, nNow))
    {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kWorkMs));
    pState->nActiveWriters.fetch_sub(1);
}

/// @brief 轮询等待条件满足（超时返回 false）。
bool WaitUntil(const std::function<bool()>& fnCond, int nTimeoutMs)
{
    const std::chrono::steady_clock::time_point tStart = std::chrono::steady_clock::now();
    while (!fnCond())
    {
        const long nElapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tStart).count();
        if (nElapsed >= nTimeoutMs)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

/// @brief 简易上下文（链的共享上下文；本文件只需要「每链一份」）。
struct SRwCtx
{
    int nId;
};

}  // namespace

/// @brief 写链之间互斥：多条 `kWrite` 链（首层即写）在同一执行器上不重叠。
TEST(AsyncRw_DefaultChainsExclusive)
{
    const int kThreads = 4;
    const int kChains = 8;

    CAsyncExecutor exec(kThreads);
    ASSERT_TRUE(exec.Start());

    SRwState state;
    std::atomic<int> nDone(0);
    std::vector<CPromise<SRwCtx> > vecChains;
    for (int i = 0; i < kChains; ++i)
    {
        std::shared_ptr<SRwCtx> spCtx = std::make_shared<SRwCtx>();
        spCtx->nId = i;
        vecChains.push_back(exec.NewPromise(
            spCtx,
            [&state, &nDone](const std::shared_ptr<SRwCtx>& /*spCtx*/)
            {
                RunWriteWork(&state);
                nDone.fetch_add(1);
                return CPromiseResult::Resolve();
            },
            TaskKind::kWrite));
    }
    for (size_t i = 0; i < vecChains.size(); ++i)
    {
        ASSERT_TRUE(vecChains[i].Await().IsFulfilled());
    }
    exec.Stop();
    ASSERT_TRUE(state.nPeakWriters.load() == 1);  // 写链互斥（默认类别 = 写）
    ASSERT_TRUE(state.nViolations.load() == 0);
}

/// @brief `Post(kRead)`：读任务之间可并发（峰值 > 1）。
TEST(AsyncRw_PostReadKindConcurrent)
{
    const int kThreads = 4;
    const int kTasks = 8;

    CAsyncExecutor exec(kThreads);
    ASSERT_TRUE(exec.Start());

    SRwState state;
    std::atomic<int> nDone(0);
    for (int i = 0; i < kTasks; ++i)
    {
        ASSERT_TRUE(exec.Post(TaskKind::kRead,
            [&state, &nDone]()
            {
                RunReadWork(&state);
                nDone.fetch_add(1);
            }));
    }

    const bool bDone = WaitUntil(
        [&nDone]()
        {
            return nDone.load() == kTasks;
        },
        5000);
    exec.Stop();

    ASSERT_TRUE(bDone);
    ASSERT_TRUE(state.nPeakReaders.load() >= 2);  // 读确实并发
    ASSERT_TRUE(state.nViolations.load() == 0);   // 读之间不互斥、也不与写重叠
}

/// @brief `Post`（默认写）：写任务互斥（峰值写 = 1）。
TEST(AsyncRw_PostWriteExclusive)
{
    const int kThreads = 4;
    const int kTasks = 8;

    CAsyncExecutor exec(kThreads);
    ASSERT_TRUE(exec.Start());

    SRwState state;
    std::atomic<int> nDone(0);
    for (int i = 0; i < kTasks; ++i)
    {
        ASSERT_TRUE(exec.Post(TaskKind::kWrite,
            [&state, &nDone]()
            {
                RunWriteWork(&state);
                nDone.fetch_add(1);
            }));
    }

    const bool bDone = WaitUntil(
        [&nDone]()
        {
            return nDone.load() == kTasks;
        },
        5000);
    exec.Stop();

    ASSERT_TRUE(bDone);
    ASSERT_TRUE(state.nPeakWriters.load() == 1);
    ASSERT_TRUE(state.nViolations.load() == 0);
}

/// @brief 读写混合：写独占、读并发，且读与写从不重叠（公平 FIFO 下的互斥）。
TEST(AsyncRw_MixedPostsNoOverlap)
{
    const int kThreads = 4;
    const int kRounds = 12;

    CAsyncExecutor exec(kThreads);
    ASSERT_TRUE(exec.Start());

    SRwState state;
    std::atomic<int> nDone(0);
    for (int i = 0; i < kRounds; ++i)
    {
        if ((i & 1) == 0)
        {
            ASSERT_TRUE(exec.Post(TaskKind::kRead,
                [&state, &nDone]()
                {
                    RunReadWork(&state);
                    nDone.fetch_add(1);
                }));
        }
        else
        {
            ASSERT_TRUE(exec.Post(TaskKind::kWrite,
                [&state, &nDone]()
                {
                    RunWriteWork(&state);
                    nDone.fetch_add(1);
                }));
        }
    }

    const bool bDone = WaitUntil(
        [&nDone]()
        {
            return nDone.load() == kRounds;
        },
        10000);
    exec.Stop();

    ASSERT_TRUE(bDone);
    ASSERT_TRUE(state.nPeakWriters.load() == 1);  // 写者唯一
    ASSERT_TRUE(state.nViolations.load() == 0);   // 读写不重叠
}

/// @brief 读链（`TaskKind::kRead`）：同一执行器上的多条读链可以并行推进。
TEST(AsyncRw_ReadChainsConcurrent)
{
    const int kThreads = 4;
    const int kChains = 8;

    CAsyncExecutor exec(kThreads);
    ASSERT_TRUE(exec.Start());

    SRwState state;
    std::atomic<int> nDone(0);
    std::vector<CPromise<SRwCtx> > vecChains;
    for (int i = 0; i < kChains; ++i)
    {
        std::shared_ptr<SRwCtx> spCtx = std::make_shared<SRwCtx>();
        spCtx->nId = i;
        vecChains.push_back(exec.NewPromise(
            spCtx,
            [&state, &nDone](const std::shared_ptr<SRwCtx>& /*spCtx*/)
            {
                RunReadWork(&state);
                nDone.fetch_add(1);
                return CPromiseResult::Resolve();
            },
            TaskKind::kRead));
    }
    for (size_t i = 0; i < vecChains.size(); ++i)
    {
        ASSERT_TRUE(vecChains[i].Await().IsFulfilled());
    }
    exec.Stop();

    ASSERT_TRUE(state.nPeakReaders.load() >= 2);  // 读链并行
    ASSERT_TRUE(state.nViolations.load() == 0);
}

/// @brief 就地级联仍然保留：单线程执行器上没有排队任务时，一条链的各层跑在同一线程上。
TEST(AsyncRw_InlineCascadeKeepsSingleThread)
{
    const int kLayers = 20;

    CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    struct CInlineCtx
    {
        std::thread::id idFirst;  // 首层的线程
        std::thread::id idLast;   // 末层的线程
        int nSteps;               // 已执行层数
    };

    std::shared_ptr<CInlineCtx> spCtx = std::make_shared<CInlineCtx>();
    spCtx->nSteps = 0;
    common::async::CPromise<CInlineCtx> tail = exec.NewPromise(
        spCtx,
        [spCtx](const std::shared_ptr<CInlineCtx>& /*spCtx*/)
        {
            spCtx->idFirst = std::this_thread::get_id();
            spCtx->nSteps += 1;
            return CPromiseResult::Resolve();
        },
        TaskKind::kWrite);
    for (int i = 1; i < kLayers; ++i)
    {
        tail = tail.Then(
            [spCtx](const std::shared_ptr<CInlineCtx>& /*spCtx*/)
            {
                spCtx->idLast = std::this_thread::get_id();
                spCtx->nSteps += 1;
                return CPromiseResult::Resolve();
            },
            TaskKind::kWrite);
    }

    ASSERT_TRUE(tail.Await().IsFulfilled());
    exec.Stop();

    ASSERT_EQ(spCtx->nSteps, kLayers);
    ASSERT_TRUE(spCtx->idFirst == spCtx->idLast);  // 无竞争时整条链在同一线程上就地跑完
}

/// @brief 停止语义：`Stop()` 拒新投递、但把已接受（含门口排队）的任务跑完 —— 不丢任务、不悬挂。
TEST(AsyncRw_StopDrainsAcceptedTasks)
{
    const int kQueued = 5;

    CAsyncExecutor exec(1);  // 单 worker：占位任务一跑，后面的任务全在门口排队
    ASSERT_TRUE(exec.Start());

    std::atomic<int> nDone(0);
    std::atomic<bool> bOccupied(false);
    std::atomic<bool> bRelease(false);
    ASSERT_TRUE(exec.Post(TaskKind::kWrite,
        [&bOccupied, &bRelease]()
        {
            bOccupied.store(true);
            while (!bRelease.load())
            {
                std::this_thread::yield();
            }
        }));
    while (!bOccupied.load())
    {
        std::this_thread::yield();
    }

    // 门口排队的任务（写，互斥 → 全部乖乖排队）
    for (int i = 0; i < kQueued; ++i)
    {
        ASSERT_TRUE(exec.Post(TaskKind::kWrite,
            [&nDone]()
            {
                nDone.fetch_add(1);
            }));
    }

    bRelease.store(true);
    exec.Stop();  // Close → Drain（把已接受的跑完）→ 停池

    ASSERT_EQ(nDone.load(), kQueued);  // 已接受的任务一个不少
    ASSERT_TRUE(exec.IsStopped());
    ASSERT_TRUE(!exec.Post(TaskKind::kWrite,
        []()
        {
        }));  // 停止后拒新投递
}

/// @brief 停止期间的链：后续层以「执行器已停」收口（链不会永久挂着）。
///
/// 门口先排一个任务，让后续层的「就地」判定不成立（队列非空 → 只能投递）——
/// 于是它必须过门，而门此时已关 → 本层收口为「执行器已停」。
TEST(AsyncRw_StopMidChainSettlesStopped)
{
    CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<SRwCtx> spCtx = std::make_shared<SRwCtx>();
    spCtx->nId = 1;

    // 首层先等「Stop 已生效」再返回（那时门必然已关）。
    CPromise<SRwCtx> chain = exec.NewPromise(
        spCtx,
        [&exec](const std::shared_ptr<SRwCtx>& /*spCtx*/) -> CPromiseResult
        {
            while (!exec.IsStopped())
            {
                std::this_thread::yield();
            }
            return CPromiseResult::Resolve();
        },
        TaskKind::kWrite);
    chain = chain.Then(
        [](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            return CPromiseResult::Resolve();
        },
        TaskKind::kWrite);

    // 排一个任务在门口（首层占着写槽位 → 它进不来）→ 后续层的就地判定不成立。
    std::atomic<int> nQueued(0);
    ASSERT_TRUE(exec.Post(TaskKind::kWrite,
        [&nQueued]()
        {
            nQueued.fetch_add(1);
        }));

    // Stop 在另一条线程上跑（它会 Drain 到首层结束，所以不能和首层在同一条线程上等）。
    std::thread threadStop(
        [&exec]()
        {
            exec.Stop();
        });
    const CPromiseResult result = chain.AwaitFor(3000);
    threadStop.join();

    ASSERT_TRUE(result.IsRejected());  // 第二层投递被拒 → 链以「执行器已停」收口
    ASSERT_TRUE(result.Message().find("执行器已停") != std::string::npos);
    ASSERT_TRUE(exec.IsStopped());
    ASSERT_EQ(nQueued.load(), 1);  // 已接受的任务照旧跑完（不丢）
}

// ====================================================================
// 逐层类别（`Then(handler, TaskKind)`）：每层各自过门，不存在继承
// ====================================================================

/// @brief 逐层类别用例的观测（工作线程只写原子）。
struct SLayerKindObs
{
    SLayerKindObs() : nReaders(0), nPeakReaders(0), nReadersSeenInWriteLayer(0)
    {}

    std::atomic<int> nReaders;                  ///< 当前读任务数。
    std::atomic<int> nPeakReaders;              ///< 读并发峰值。
    std::atomic<int> nReadersSeenInWriteLayer;  ///< 进入写层时还有读者的次数（应为 0）。

    /// @brief 进入读层（并记峰值）。
    void EnterRead()
    {
        const int nNow = nReaders.fetch_add(1) + 1;
        int nPeak = nPeakReaders.load();
        while (nPeak < nNow && !nPeakReaders.compare_exchange_weak(nPeak, nNow))
        {
        }
    }

    /// @brief 离开读层。
    void LeaveRead()
    {
        nReaders.fetch_sub(1);
    }

    /// @brief 进写层：此刻若还有读者在场就记账（读 / 写不得重叠）。
    void EnterWrite()
    {
        if (nReaders.load() > 0)
        {
            nReadersSeenInWriteLayer.fetch_add(1);
        }
    }
};

/// @brief 逐段类别用例的协程：首段 + 一个「等待 → 恢复」段（类别各自声明）。
///
/// 段内自检「任务帧的类别」并记进原子（主线程断言）——任务帧是读写门过门的凭据，
/// 它比「跑起来没有」更能说明这一段是以什么身份进入模块的。
class CSegmentKindCoro : public common::async::CCoroutine<SRwCtx>
{
public:
    /// @param spCtx 共享上下文。
    /// @param pObsIn 读并发观测。
    /// @param promiseGateIn 被等待的 promise（由外部 settle；段类别与它无关）。
    /// @param pDoneIn 恢复段跑完置位（主线程带超时等它，实现坏了也不会挂住用例）。
    CSegmentKindCoro(const std::shared_ptr<SRwCtx>& spCtx, SLayerKindObs* pObsIn, const CPromise<SRwCtx>& promiseGateIn,
        std::atomic<bool>* pDoneIn)
        : common::async::CCoroutine<SRwCtx>(spCtx),
          pObs(pObsIn),
          promiseGate(promiseGateIn),
          pDone(pDoneIn),
          nFirstKind(-1),
          nResumeKind(-1)
    {}

    void Run() override
    {
        CO_BEGIN();
        {
            const common::async::detail::CTaskFrame* pFrame = common::async::detail::CTaskFrame::Top();
            nFirstKind.store(pFrame != NULL ? static_cast<int>(pFrame->eKind) : -1);
        }
        // 恢复后那一段显式声明为读（本段只读上下文 / 观测计数，不碰模块状态）。
        CO_AWAIT(common::async::TaskKind::kRead, promiseGate);
        {
            const common::async::detail::CTaskFrame* pFrame = common::async::detail::CTaskFrame::Top();
            nResumeKind.store(pFrame != NULL ? static_cast<int>(pFrame->eKind) : -1);
        }
        pObs->EnterRead();
        std::this_thread::sleep_for(std::chrono::milliseconds(kWorkMs * 2));
        pObs->LeaveRead();
        if (pDone != NULL)
        {
            pDone->store(true);
        }
        CO_RETURN_VOID();
        CO_END();
    }

    SLayerKindObs* pObs;           ///< 读并发观测。
    CPromise<SRwCtx> promiseGate;  ///< 被等待的 promise。
    std::atomic<bool>* pDone;      ///< 恢复段跑完置位。
    std::atomic<int> nFirstKind;   ///< 首段跑在什么类别的任务帧里（-1 = 没有帧）。
    std::atomic<int> nResumeKind;  ///< 恢复段跑在什么类别的任务帧里（-1 = 没有帧）。
};


/// @brief 写链里的「读层」（显式 `kRead`）：可与别的读任务并发。
///
/// 构造是确定的：先起写链并等它跑起来，再把一个纯读任务排在它后面 ——
/// 写层结束时纯读被放行，与写链的第二层（读层）同时跑。
/// 若这一层误标成写（两层就会互斥：读并发峰值恒为 1），本用例失败。
TEST(AsyncRw_PerLayerReadInWriteChain)
{
    const int kThreads = 4;

    CAsyncExecutor exec(kThreads);
    ASSERT_TRUE(exec.Start());

    SLayerKindObs obs;
    std::atomic<bool> bWriteLayerRunning(false);
    std::atomic<int> nDone(0);

    std::shared_ptr<SRwCtx> spChainCtx = std::make_shared<SRwCtx>();
    spChainCtx->nId = 1;
    // 链级 = 写（默认）；首层写、第二层显式标读。
    CPromise<SRwCtx> chain = exec.NewPromise(
        spChainCtx,
        [&obs, &bWriteLayerRunning](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            bWriteLayerRunning.store(true);  // 让主线程知道写层已占住门
            std::this_thread::sleep_for(std::chrono::milliseconds(kWorkMs));
            obs.EnterWrite();
            return CPromiseResult::Resolve();
        },
        TaskKind::kWrite);

    ASSERT_TRUE(WaitUntil(
        [&bWriteLayerRunning]()
        {
            return bWriteLayerRunning.load();
        },
        1000));

    // 纯读任务：写层还在跑 → 它只能排队；写层一结束它就被放行。
    std::shared_ptr<SRwCtx> spReadCtx = std::make_shared<SRwCtx>();
    spReadCtx->nId = 2;
    CPromise<SRwCtx> readChain = exec.NewPromise(
        spReadCtx,
        [&obs, &nDone](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            obs.EnterRead();
            std::this_thread::sleep_for(std::chrono::milliseconds(kWorkMs * 2));
            obs.LeaveRead();
            nDone.fetch_add(1);
            return CPromiseResult::Resolve();
        },
        TaskKind::kRead);

    // 写链第二层：显式 kRead（本层跟着纯读一起跑）。
    chain = chain.Then(
        [&obs, &nDone](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            obs.EnterRead();
            std::this_thread::sleep_for(std::chrono::milliseconds(kWorkMs * 2));
            obs.LeaveRead();
            nDone.fetch_add(1);
            return CPromiseResult::Resolve();
        },
        TaskKind::kRead, ASYNC_LOC);

    ASSERT_TRUE(chain.Await().IsFulfilled());
    ASSERT_TRUE(readChain.Await().IsFulfilled());
    exec.Stop();

    ASSERT_EQ(nDone.load(), 2);
    ASSERT_TRUE(obs.nPeakReaders.load() >= 2);  // 读层与纯读真的重叠了（否则本层还是按写过门）
}

/// @brief 读链里的「写层」（显式 `kWrite`）：它独占 —— 执行期间读者必须已排空。
///
/// 构造：读链的首层（读，短）+ 一个长纯读任务并排跑；读链第二层标写 →
/// 它必须等那个长读者结束才能开始。若这一层误标成读，它会被立即放行，
/// 于是「进入写层时还有读者在场」就会被记上。
TEST(AsyncRw_PerLayerWriteInReadChain)
{
    const int kThreads = 4;
    const int kLongReadMs = kWorkMs * 6;  // 长读者：保证写层开始时它还在跑

    CAsyncExecutor exec(kThreads);
    ASSERT_TRUE(exec.Start());

    SLayerKindObs obs;
    std::atomic<int> nDone(0);

    std::shared_ptr<SRwCtx> spChainCtx = std::make_shared<SRwCtx>();
    spChainCtx->nId = 1;
    // 链级 = 读；首层读、第二层显式标写。
    CPromise<SRwCtx> chain = exec.NewPromise(
        spChainCtx,
        [&obs, &nDone](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            obs.EnterRead();
            std::this_thread::sleep_for(std::chrono::milliseconds(kWorkMs));
            obs.LeaveRead();
            nDone.fetch_add(1);
            return CPromiseResult::Resolve();
        },
        TaskKind::kRead);

    // 长读者：与读链首层一起跑，写层必须等它结束。
    std::shared_ptr<SRwCtx> spLongCtx = std::make_shared<SRwCtx>();
    spLongCtx->nId = 2;
    CPromise<SRwCtx> longRead = exec.NewPromise(
        spLongCtx,
        [&obs, &nDone, kLongReadMs](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            obs.EnterRead();
            std::this_thread::sleep_for(std::chrono::milliseconds(kLongReadMs));
            obs.LeaveRead();
            nDone.fetch_add(1);
            return CPromiseResult::Resolve();
        },
        TaskKind::kRead);

    chain = chain.Then(
        [&obs, &nDone](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            obs.EnterWrite();  // 此刻若有读者在场 → 记账（真正独占时恒为 0）
            std::this_thread::sleep_for(std::chrono::milliseconds(kWorkMs));
            nDone.fetch_add(1);
            return CPromiseResult::Resolve();
        },
        TaskKind::kWrite, ASYNC_LOC);

    ASSERT_TRUE(chain.Await().IsFulfilled());
    ASSERT_TRUE(longRead.Await().IsFulfilled());
    exec.Stop();

    ASSERT_EQ(nDone.load(), 3);
    ASSERT_EQ(obs.nReadersSeenInWriteLayer.load(), 0);  // 写层独占：读者已排空
}

/// @brief 读链的每一层都显式标读：层之间可以并发（每层各自过门，不靠继承）。
TEST(AsyncRw_AllLayersMarkedReadConcurrent)
{
    const int kThreads = 4;
    const int kChains = 6;
    const int kLayers = 2;

    CAsyncExecutor exec(kThreads);
    ASSERT_TRUE(exec.Start());

    SLayerKindObs obs;
    std::atomic<int> nDone(0);
    std::vector<CPromise<SRwCtx> > vecChains;
    for (int i = 0; i < kChains; ++i)
    {
        std::shared_ptr<SRwCtx> spCtx = std::make_shared<SRwCtx>();
        spCtx->nId = i;
        CPromise<SRwCtx> chain = exec.NewPromise(
            spCtx,
            [&obs, &nDone](const std::shared_ptr<SRwCtx>& /*spCtx*/)
            {
                obs.EnterRead();
                std::this_thread::sleep_for(std::chrono::milliseconds(kWorkMs));
                obs.LeaveRead();
                nDone.fetch_add(1);
                return CPromiseResult::Resolve();
            },
            TaskKind::kRead);
        for (int nLayer = 1; nLayer < kLayers; ++nLayer)
        {
            chain = chain.Then(
                [&obs, &nDone](const std::shared_ptr<SRwCtx>& /*spCtx*/)
                {
                    obs.EnterRead();
                    std::this_thread::sleep_for(std::chrono::milliseconds(kWorkMs));
                    obs.LeaveRead();
                    nDone.fetch_add(1);
                    return CPromiseResult::Resolve();
                },
                TaskKind::kRead);
        }
        vecChains.push_back(chain);
    }
    for (size_t i = 0; i < vecChains.size(); ++i)
    {
        ASSERT_TRUE(vecChains[i].Await().IsFulfilled());
    }
    exec.Stop();

    ASSERT_EQ(nDone.load(), kChains * kLayers);
    ASSERT_TRUE(obs.nPeakReaders.load() >= 2);  // 读层之间真的重叠了
}

/// @brief 「等子链」那一层（`ThenPromise` / `ThenBridge`）的工厂也要过门。
///
/// 场景：上游是**外部线程 settle** 的（起链回调把兑现函数交给别的线程）—— 登记路径若直接调工厂，
/// 工厂就会在那个外部线程上、**无槽位**地跑（声明的类别形同虚设）。本测试钉住：工厂拿到本层
/// `kWrite` 的槽位，且**不在**结算线程上跑。
TEST(AsyncRw_BridgeFactoryRunsGated)
{
    CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::atomic<bool> bFactoryRan(false);
    std::atomic<bool> bFactoryHasSlot(false);
    std::atomic<bool> bSettled(false);
    std::atomic<bool> bStarterRan(false);  // 起链回调已登记好 fnSettleLater
    std::thread::id factoryTid;            // 只由工厂写，主线程在等完 bFactoryRan 之后读
    std::thread::id settlerTid;            // 只由结算线程写，主线程 join 之后读

    std::function<void()> fnSettleLater;
    std::shared_ptr<SRwCtx> spCtx = std::make_shared<SRwCtx>();
    spCtx->nId = 1;
    CPromise<SRwCtx> chain = exec.NewPromise(
        spCtx,
        [&fnSettleLater, &bStarterRan](
            const CPromise<SRwCtx>::ResolveFn& fnResolve, const CPromise<SRwCtx>::RejectFn& /*fnReject*/)
        {
            // 起链回调：只登记，稍后由外部线程 settle（模拟别的模块回调）。
            fnSettleLater = [fnResolve]()
            {
                fnResolve();  // ResolveFn = void()
            };
            bStarterRan.store(true);  // 登记完成：外部线程才能读 fnSettleLater
        },
        TaskKind::kRead);

    chain = chain.ThenPromise(
        [&exec, &bFactoryRan, &bFactoryHasSlot, &factoryTid](const std::shared_ptr<SRwCtx>& spSelf) -> CPromise<SRwCtx>
        {
            bFactoryHasSlot.store(common::async::detail::CTaskFrame::Top() != NULL);
            factoryTid = std::this_thread::get_id();
            bFactoryRan.store(true);
            return exec.NewPromise(
                spSelf,
                [](const std::shared_ptr<SRwCtx>& /*spCtx*/)
                {
                    return CPromiseResult::Resolve();
                },
                TaskKind::kRead);
        },
        TaskKind::kWrite);

    // 起链回调现在按类别过门投递（门外起链也不再在调用线程上同步跑）→ 等它登记完再开结算线程。
    ASSERT_TRUE(WaitUntil(
        [&bStarterRan]()
        {
            return bStarterRan.load();
        },
        2000));

    std::thread settler(
        [&fnSettleLater, &settlerTid, &bSettled]()
        {
            settlerTid = std::this_thread::get_id();
            fnSettleLater();
            bSettled.store(true);  // 结算已提交（工厂可能还没跑）
        });

    ASSERT_TRUE(WaitUntil(
        [&bFactoryRan]()
        {
            return bFactoryRan.load();
        },
        2000));
    settler.join();

    ASSERT_TRUE(bFactoryHasSlot.load());    // 工厂过门：拿到本层 kWrite 的槽位
    ASSERT_TRUE(factoryTid != settlerTid);  // 且不在结算线程上（回到本链执行器线程）
    ASSERT_TRUE(bSettled.load());
    exec.Stop();
}

/// @brief 跨模块起链：被调模块的起链回调跑在**它自己的门**里（不是调用方那扇门）。
///
/// 场景：调用方在自己的写槽位里调被调模块的公开异步函数（`NewPromise(spCtx, fnStarter, 类别)`）——
/// 「门外（对 B 而言）/ 别的门」起链一律**按声明类别过门投递**后再跑，所以：
///  - 起链回调拿到的是**被调模块自己**的槽位（碰它自己的状态是安全的）；
///  - 类别 = 它声明的类别（不再是「继承调用方那扇门」）。
TEST(AsyncRw_CrossModuleStarterLandsInCalleeGate)
{
    CAsyncExecutor execCaller(2);  // 调用方模块（自己的门）
    CAsyncExecutor execCallee(2);  // 被调模块（自己的门）
    ASSERT_TRUE(execCaller.Start());
    ASSERT_TRUE(execCallee.Start());

    // 取「被调模块的门」：在它的门内任务里读一次帧（帧里带着所属门）。
    const void* pGateCallee = NULL;
    std::atomic<bool> bGotGate(false);
    ASSERT_TRUE(execCallee.Post(TaskKind::kWrite,
        [&pGateCallee, &bGotGate]()
        {
            pGateCallee = common::async::detail::CTaskFrame::Top()->pGate;
            bGotGate.store(true);
        }));
    ASSERT_TRUE(WaitUntil(
        [&bGotGate]()
        {
            return bGotGate.load();
        },
        1000));

    std::shared_ptr<SRwCtx> spCalleeCtx = std::make_shared<SRwCtx>();
    spCalleeCtx->nId = 9;
    std::atomic<bool> bStarterRan(false);
    std::atomic<bool> bStarterInOwnGate(false);
    std::atomic<bool> bStarterHasSlot(false);
    std::atomic<int> nStarterKind(-1);
    std::atomic<bool> bChildSettled(false);

    // 调用方在自己的写槽位里调被调模块的异步函数（跨模块起链）
    ASSERT_TRUE(execCaller.Post(TaskKind::kWrite,
        [&execCallee, spCalleeCtx, &pGateCallee, &bStarterRan, &bStarterInOwnGate, &bStarterHasSlot, &nStarterKind,
            &bChildSettled]()
        {
            CPromise<SRwCtx> promiseCallee = execCallee.NewPromise(
                spCalleeCtx,
                [&bStarterRan, &bStarterInOwnGate, &bStarterHasSlot, &nStarterKind, &pGateCallee](
                    const CPromise<SRwCtx>::ResolveFn& fnResolve, const CPromise<SRwCtx>::RejectFn& /*fnReject*/)
                {
                    const common::async::detail::CTaskFrame* pFrame = common::async::detail::CTaskFrame::Top();
                    bStarterHasSlot.store(pFrame != NULL);
                    bStarterInOwnGate.store(pFrame != NULL && pFrame->pGate == pGateCallee);
                    nStarterKind.store(pFrame != NULL ? static_cast<int>(pFrame->eKind) : -1);
                    bStarterRan.store(true);
                    fnResolve();
                },
                TaskKind::kWrite);
            // 跨模块链照常可续接（本层由子链落定收口）
            promiseCallee.OnSettled(
                [&bChildSettled](common::async::CPromiseResult /*result*/)
                {
                    bChildSettled.store(true);
                });
        }));

    ASSERT_TRUE(WaitUntil(
        [&bStarterRan]()
        {
            return bStarterRan.load();
        },
        2000));
    ASSERT_TRUE(bStarterHasSlot.load());
    ASSERT_TRUE(bStarterInOwnGate.load());                               // 在被调模块自己的门里
    ASSERT_EQ(nStarterKind.load(), static_cast<int>(TaskKind::kWrite));  // 类别 = 声明的类别
    ASSERT_TRUE(WaitUntil(
        [&bChildSettled]()
        {
            return bChildSettled.load();
        },
        2000));
    execCaller.Stop();
    execCallee.Stop();
}

/// @brief 起链回调的类别是**有效**的：它决定「起链回调以什么身份进模块」。
///
/// 场景：在**写槽位**里起一条**声明为读**的链 —— 换类别不能就地（那等于用写槽位跑读身份的活），
/// 于是按读类别过门投递；起链回调跑起来时，帧上的类别就是它声明的「读」。
TEST(AsyncRw_StarterRunsUnderDeclaredKind)
{
    CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<SRwCtx> spCtx = std::make_shared<SRwCtx>();
    spCtx->nId = 3;
    std::atomic<bool> bDone(false);
    std::atomic<bool> bHasSlot(false);
    std::atomic<int> nKind(-1);

    ASSERT_TRUE(exec.Post(TaskKind::kWrite,
        [&exec, spCtx, &bDone, &bHasSlot, &nKind]()
        {
            exec.NewPromise(
                spCtx,
                [&bDone, &bHasSlot, &nKind](
                    const CPromise<SRwCtx>::ResolveFn& fnResolve, const CPromise<SRwCtx>::RejectFn& /*fnReject*/)
                {
                    const common::async::detail::CTaskFrame* pFrame = common::async::detail::CTaskFrame::Top();
                    bHasSlot.store(pFrame != NULL);
                    nKind.store(pFrame != NULL ? static_cast<int>(pFrame->eKind) : -1);
                    fnResolve();
                    bDone.store(true);
                },
                TaskKind::kRead);  // 声明读：在写槽位里换类别 → 过门投递后再跑
        }));

    ASSERT_TRUE(WaitUntil(
        [&bDone]()
        {
            return bDone.load();
        },
        2000));
    ASSERT_TRUE(bHasSlot.load());
    ASSERT_EQ(nKind.load(), static_cast<int>(TaskKind::kRead));
    exec.Stop();
}

/// @brief `ASYNC_GATE`：挂层处类别与函数体声明**一致**时零成本落穿（不挂起、不重入）。
TEST(AsyncRw_GateMacroNoOpWhenKindMatches)
{
    CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::atomic<int> nBodyRuns(0);
    std::atomic<int> nBodyKind(-1);
    std::shared_ptr<SRwCtx> spCtx = std::make_shared<SRwCtx>();
    spCtx->nId = 1;

    // 挂层处 kWrite + 函数体声明 kWrite：宏直接落穿（本层只跑一次，是一次普通层执行）。
    const CPromiseResult result = exec.NewPromise(
                                          spCtx,
                                          [](const std::shared_ptr<SRwCtx>& /*spCtx*/)
                                          {
                                              return CPromiseResult::Resolve();
                                          },
                                          TaskKind::kRead, ASYNC_LOC)
                                      .Then(
                                          [&nBodyRuns, &nBodyKind](const std::shared_ptr<SRwCtx>& /*spCtx*/)
                                          {
                                              ASYNC_GATE_READ();  // 与挂层处同类
                                              ++nBodyRuns;
                                              nBodyKind.store(static_cast<int>(common::async::detail::CTaskFrame::Top()->eKind));
                                              return CPromiseResult::Resolve();
                                          },
                                          TaskKind::kRead, ASYNC_LOC)
                                      .Then(
                                          [](const std::shared_ptr<SRwCtx>& /*spCtx*/)
                                          {
                                              return CPromiseResult::Resolve();
                                          },
                                          TaskKind::kWrite, ASYNC_LOC)
                                      .Await();

    ASSERT_TRUE(result.IsFulfilled());
    ASSERT_EQ(nBodyRuns.load(), 1);                                  // 没有重入
    ASSERT_EQ(nBodyKind.load(), static_cast<int>(TaskKind::kRead));  // 在声明的读槽位里跑
    exec.Stop();
}

/// @brief `ASYNC_GATE`：挂层处给读、函数体要写 → 挂起 + 按写类别过门重入（真等读者排空）。
///
/// 断言三件事：
///  - 首次（读槽位里）只跑到宏那一行就返回 → 宏之后的函数体**一次都没跑**；
///  - 本层没 settle → 下游层不提前跑（链序保持）；
///  - 长读任务占着读槽位时，升级后的写层**进不来**（放行后才跑，且帧类别 = 写）。
TEST(AsyncRw_GateMacroUpgradesToWriteSlot)
{
    CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    std::atomic<bool> bLongReadHolds(false);
    std::atomic<bool> bReleaseLongRead(false);
    std::atomic<bool> bFirstLayerRan(false);
    std::atomic<int> nBodyRuns(0);
    std::atomic<int> nBodyKind(-1);
    std::atomic<bool> bDownstream(false);

    // 长读任务占住一个读槽位（由主线程放行）
    ASSERT_TRUE(exec.Post(TaskKind::kRead,
        [&bLongReadHolds, &bReleaseLongRead]()
        {
            bLongReadHolds.store(true);
            WaitUntil(
                [&bReleaseLongRead]()
                {
                    return bReleaseLongRead.load();
                },
                2000);
        }));
    ASSERT_TRUE(WaitUntil(
        [&bLongReadHolds]()
        {
            return bLongReadHolds.load();
        },
        1000));

    std::shared_ptr<SRwCtx> spCtx = std::make_shared<SRwCtx>();
    spCtx->nId = 2;
    CPromise<SRwCtx> chain = exec.NewPromise(
                                     spCtx,
                                     [&bFirstLayerRan](const std::shared_ptr<SRwCtx>& /*spCtx*/)
                                     {
                                         bFirstLayerRan.store(true);
                                         return CPromiseResult::Resolve();
                                     },
                                     TaskKind::kRead, ASYNC_LOC)
                                 .Then(
                                     [&nBodyRuns, &nBodyKind](const std::shared_ptr<SRwCtx>& /*spCtx*/)
                                     {
                                         // 升级点：调用点**连类别都不写**（缺省不过门），函数体声明「本层体必须独占」
                                         ASYNC_GATE_WRITE();
                                         ++nBodyRuns;
                                         nBodyKind.store(static_cast<int>(common::async::detail::CTaskFrame::Top()->eKind));
                                         return CPromiseResult::Resolve();
                                     })  // ← 省略类别：默认不过门，门要求由上面的 ASYNC_GATE 声明
                                 .Then(
                                     [&bDownstream](const std::shared_ptr<SRwCtx>& /*spCtx*/)
                                     {
                                         bDownstream.store(true);
                                         return CPromiseResult::Resolve();
                                     },
                                     TaskKind::kWrite, ASYNC_LOC);

    ASSERT_TRUE(WaitUntil(
        [&bFirstLayerRan]()
        {
            return bFirstLayerRan.load();
        },
        1000));
    // 读槽位被长读任务占着：升级的写层进不来 → 宏之后的函数体还没跑、下游也没跑
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_EQ(nBodyRuns.load(), 0);
    ASSERT_TRUE(!bDownstream.load());

    bReleaseLongRead.store(true);  // 放行读者 → 写层过门
    ASSERT_TRUE(chain.Await().IsFulfilled());
    ASSERT_EQ(nBodyRuns.load(), 1);                                   // 只跑一次（重入后宏落穿）
    ASSERT_EQ(nBodyKind.load(), static_cast<int>(TaskKind::kWrite));  // 在写槽位里
    ASSERT_TRUE(bDownstream.load());                                  // 本层 settle 之后才轮到下游
    exec.Stop();
}

/// @brief `Then` 省略类别 = **不过门**（缺省 `kDirect`）：写者占着门时它照跑，且不占槽位。
TEST(AsyncRw_ThenWithoutKindRunsUngated)
{
    CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::atomic<bool> bWriterHolds(false);
    std::atomic<bool> bReleaseWriter(false);
    std::atomic<bool> bUngatedRan(false);
    std::atomic<bool> bWriterStillHolding(false);
    std::atomic<int> nFrameKind(-2);  // -1 = 没有任务帧（不过门）

    // 写任务占住门不放
    ASSERT_TRUE(exec.Post(TaskKind::kWrite,
        [&bWriterHolds, &bReleaseWriter]()
        {
            bWriterHolds.store(true);
            WaitUntil(
                [&bReleaseWriter]()
                {
                    return bReleaseWriter.load();
                },
                2000);
        }));
    ASSERT_TRUE(WaitUntil(
        [&bWriterHolds]()
        {
            return bWriterHolds.load();
        },
        1000));

    std::shared_ptr<SRwCtx> spCtx = std::make_shared<SRwCtx>();
    spCtx->nId = 5;
    CPromise<SRwCtx> chain =
        exec.NewPromise(
                spCtx,
                [](const std::shared_ptr<SRwCtx>& /*spCtx*/)
                {
                    return CPromiseResult::Resolve();
                },
                TaskKind::kDirect, ASYNC_LOC)  // 首层显式直投（写者占门时也能跑，便于观察默认层）
            .Then(
                [&bUngatedRan, &bWriterHolds, &bWriterStillHolding, &nFrameKind](const std::shared_ptr<SRwCtx>& /*spCtx*/)
                {
                    // 省略类别 → 缺省不过门：没有任务帧、且写者仍在门里
                    const common::async::detail::CTaskFrame* pFrame = common::async::detail::CTaskFrame::Top();
                    nFrameKind.store(pFrame != NULL ? static_cast<int>(pFrame->eKind) : -1);
                    bWriterStillHolding.store(bWriterHolds.load());
                    bUngatedRan.store(true);
                    return CPromiseResult::Resolve();
                });  // ← 省略类别

    ASSERT_TRUE(WaitUntil(
        [&bUngatedRan]()
        {
            return bUngatedRan.load();
        },
        1000));                        // 写者占门也照跑 → 确实不过门
    ASSERT_EQ(nFrameKind.load(), -1);  // 没有槽位（帧为空）
    ASSERT_TRUE(bWriterStillHolding.load());
    bReleaseWriter.store(true);
    ASSERT_TRUE(chain.Await().IsFulfilled());
    exec.Stop();
}

// ====================================================================
// `ASYNC_GATE` 用法样例（**具名处理器**，与业务模块里的写法一致）
//
// 三种形态都演示一遍：
//   ① ASYNC_GATE_WRITE()   —— 会改状态，要独占
//   ② ASYNC_GATE_READ()    —— 只读，可与其它读并发
//   ③ ASYNC_GATE(kWrite)   —— 基础形态（类别用宏参数给）
// 调用点**都不写类别**（缺省不过门），门要求由函数体自己声明。
// ====================================================================

namespace {

/// @brief 样例上下文：记「函数体跑了几次 + 以什么类别跑的」。
struct SGateSampleCtx
{
    SGateSampleCtx() : nRuns(0), nKind(-2)
    {}

    std::atomic<int> nRuns;  ///< 函数体跑了几次（重入落穿后仍应为 1）。
    std::atomic<int> nKind;  ///< 帧上的类别（-1 = 没有帧 / 不过门）。
};

/// @brief 记一笔「本层体是在什么槽位里跑的」（三种样例共用）。
///
/// @param spCtx 样例上下文。
/// @param eKind 本次帧上的类别（没有帧则传 `kDirect`）。
void NoteRun(const std::shared_ptr<SGateSampleCtx>& spCtx, const common::async::detail::CTaskFrame* pFrame)
{
    spCtx->nRuns.fetch_add(1);
    spCtx->nKind.store(pFrame != nullptr ? static_cast<int>(pFrame->eKind) : -1);
}

/// @brief 用法样例①：具名处理器 + `ASYNC_GATE_WRITE()`（改状态 → 要写槽位）。
///
/// @param spCtx 样例上下文。
/// @return 兑现。
common::async::CPromiseResult StepSampleWriteGated(const std::shared_ptr<SGateSampleCtx>& spCtx)
{
    ASYNC_GATE_WRITE();  // ← 必须第一条语句：本函数体独占进入本模块
    NoteRun(spCtx, common::async::detail::CTaskFrame::Top());
    return common::async::CPromiseResult::Resolve();
}

/// @brief 用法样例②：具名处理器 + `ASYNC_GATE_READ()`（只读 → 与其它读并发）。
///
/// @param spCtx 样例上下文。
/// @return 兑现。
common::async::CPromiseResult StepSampleReadGated(const std::shared_ptr<SGateSampleCtx>& spCtx)
{
    ASYNC_GATE_READ();  // ← 必须第一条语句：本函数体在读槽位里跑
    NoteRun(spCtx, common::async::detail::CTaskFrame::Top());
    return common::async::CPromiseResult::Resolve();
}

/// @brief 用法样例③：基础形态 `ASYNC_GATE(kWrite)`（类别当宏参数给）。
///
/// @param spCtx 样例上下文。
/// @return 兑现。
common::async::CPromiseResult StepSampleExplicitGated(const std::shared_ptr<SGateSampleCtx>& spCtx)
{
    ASYNC_GATE(kWrite);  // ← 基础形态；`ASYNC_GATE_READ()` / `ASYNC_GATE_WRITE()` 就是它的简写
    NoteRun(spCtx, common::async::detail::CTaskFrame::Top());
    return common::async::CPromiseResult::Resolve();
}

/// @brief 起一条「首层（读）→ 样例处理器（不写类别）」的链并等它跑完。
///
/// @param exec 执行器。
/// @param spCtx 样例上下文。
/// @param fnHandler 样例处理器。
/// @return 链的最终结果。
common::async::CPromiseResult RunSampleChain(common::async::CAsyncExecutor& exec, const std::shared_ptr<SGateSampleCtx>& spCtx,
    const std::function<common::async::CPromiseResult(const std::shared_ptr<SGateSampleCtx>&)>& fnHandler)
{
    return exec
        .NewPromise(
            spCtx,
            [](const std::shared_ptr<SGateSampleCtx>& /*spCtx*/)
            {
                return common::async::CPromiseResult::Resolve();
            },
            TaskKind::kRead, ASYNC_LOC)
        .Then(fnHandler)  // ← 省略类别：缺省不过门，门要求由处理器自己声明
        .Await();
}

}  // namespace

/// @brief `ASYNC_GATE` 用法样例：三种形态都能「调用点不写类别、函数体自己声明门要求」。
TEST(AsyncRw_GateMacroUsageSamples)
{
    CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    // ① 写声明：链上首层是读、本层省略类别 → 自动升级到写槽位
    {
        std::shared_ptr<SGateSampleCtx> spCtx = std::make_shared<SGateSampleCtx>();
        ASSERT_TRUE(RunSampleChain(exec, spCtx, &StepSampleWriteGated).IsFulfilled());
        ASSERT_EQ(spCtx->nRuns.load(), 1);  // 重入落穿后只跑一次函数体
        ASSERT_EQ(spCtx->nKind.load(), static_cast<int>(TaskKind::kWrite));
    }

    // ② 读声明：在读槽位里跑
    {
        std::shared_ptr<SGateSampleCtx> spCtx = std::make_shared<SGateSampleCtx>();
        ASSERT_TRUE(RunSampleChain(exec, spCtx, &StepSampleReadGated).IsFulfilled());
        ASSERT_EQ(spCtx->nRuns.load(), 1);
        ASSERT_EQ(spCtx->nKind.load(), static_cast<int>(TaskKind::kRead));
    }

    // ③ 基础形态：同上（写成 ASYNC_GATE(kWrite)）
    {
        std::shared_ptr<SGateSampleCtx> spCtx = std::make_shared<SGateSampleCtx>();
        ASSERT_TRUE(RunSampleChain(exec, spCtx, &StepSampleExplicitGated).IsFulfilled());
        ASSERT_EQ(spCtx->nRuns.load(), 1);
        ASSERT_EQ(spCtx->nKind.load(), static_cast<int>(TaskKind::kWrite));
    }

    exec.Stop();
}

#if defined(ASYNC_DEBUG_TRACE)
/// @brief 逐层类别在 trace 里看得到（`CLayerInfo::eKind`）：层里自检「我这层是读还是写」。
TEST(AsyncRw_PerLayerKindVisibleInTrace)
{
    CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<SRwCtx> spCtx = std::make_shared<SRwCtx>();
    spCtx->nId = 1;
    std::atomic<int> nReadLayerKind(0);
    std::atomic<int> nWriteLayerKind(0);

    CPromise<SRwCtx> chain = exec.NewPromise(
        spCtx,
        [&nWriteLayerKind](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            const common::async::CLayerInfo* pInfo = common::async::CurrentLayer();
            if (pInfo != NULL && pInfo->eKind == TaskKind::kWrite)
            {
                nWriteLayerKind.fetch_add(1);
            }
            return CPromiseResult::Resolve();
        },
        TaskKind::kWrite);
    chain = chain.Then(
        [&nReadLayerKind](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            const common::async::CLayerInfo* pInfo = common::async::CurrentLayer();
            if (pInfo != NULL && pInfo->eKind == TaskKind::kRead)
            {
                nReadLayerKind.fetch_add(1);
            }
            return CPromiseResult::Resolve();
        },
        TaskKind::kRead, ASYNC_LOC);

    ASSERT_TRUE(chain.Await().IsFulfilled());
    exec.Stop();

    ASSERT_EQ(nWriteLayerKind.load(), 1);  // 链级默认写
    ASSERT_EQ(nReadLayerKind.load(), 1);   // 本层显式读
}
#endif  // defined(ASYNC_DEBUG_TRACE)

// ====================================================================
// 协程的类别逐段给：CoStart = 首段，每个 CO_AWAIT = 恢复后那一段
// ====================================================================

/// @brief 协程「逐段类别」：`CoStart` 定首段、`CO_AWAIT` 定恢复段，两段都真的按该类别过门。
///
/// 构造是确定的：一个长读任务占住读槽位不放（等主线程放行），协程首段与恢复段都声明为读 →
/// 两段都能与它并发（读峰值 ≥ 2），且段内读到的任务帧类别就是 `kRead`。
/// 若恢复段被当成写（要等读者排空 → 本用例超时）或直投（帧为空），断言即失败。
TEST(AsyncRw_CoroutineSegmentKindGuarded)
{
    const int kThreads = 4;

    CAsyncExecutor exec(kThreads);
    ASSERT_TRUE(exec.Start());

    SLayerKindObs obs;
    std::atomic<bool> bLongReadInside(false);
    std::atomic<bool> bReleaseLongRead(false);

    // 长读任务：占住一个读槽位，直到主线程放行（协程两段都应能与它并发）。
    ASSERT_TRUE(exec.Post(TaskKind::kRead,
        [&obs, &bLongReadInside, &bReleaseLongRead]()
        {
            obs.EnterRead();
            bLongReadInside.store(true);
            WaitUntil(
                [&bReleaseLongRead]()
                {
                    return bReleaseLongRead.load();
                },
                2000);
            obs.LeaveRead();
        }));
    ASSERT_TRUE(WaitUntil(
        [&bLongReadInside]()
        {
            return bLongReadInside.load();
        },
        1000));

    // 门桩：一条「当场兑现」的 promise（只负责让协程走一次「挂起 → 恢复」）。
    // 门外起链按**声明类别过门**后再跑：桩要与上面那条长读任务并存，所以声明读
    // （若声明 kWrite，它会等到读者排空 —— 那正是写类别的语义，桩会被长读任务挡住）。
    std::shared_ptr<SRwCtx> spCoroCtx = std::make_shared<SRwCtx>();
    spCoroCtx->nId = 2;
    CPromise<SRwCtx> promiseGate = exec.NewPromise(
        spCoroCtx,
        [](const CPromise<SRwCtx>::ResolveFn& fnResolve, const CPromise<SRwCtx>::RejectFn& /*fnReject*/)
        {
            fnResolve();
        },
        TaskKind::kRead);

    // 首段 = 读（CoStart 的类别）；恢复段 = 读（CO_AWAIT 的类别）。
    std::atomic<bool> bCoroDone(false);
    std::shared_ptr<CSegmentKindCoro> pCoro =
        exec.CoStart<CSegmentKindCoro>(TaskKind::kRead, spCoroCtx, &obs, promiseGate, &bCoroDone);

    // 带超时等协程跑完：实现坏了（恢复段被卡住）也只是用例失败，不会挂住。
    ASSERT_TRUE(WaitUntil(
        [&bCoroDone]()
        {
            return bCoroDone.load();
        },
        1500));

    const CPromiseResult result = pCoro->Await();
    bReleaseLongRead.store(true);
    exec.Stop();

    ASSERT_TRUE(result.IsFulfilled());
    ASSERT_TRUE(obs.nPeakReaders.load() >= 2);                                // 恢复段与长读任务真的重叠了
    ASSERT_EQ(pCoro->nFirstKind.load(), static_cast<int>(TaskKind::kRead));   // 首段：读帧
    ASSERT_EQ(pCoro->nResumeKind.load(), static_cast<int>(TaskKind::kRead));  // 恢复段：读帧
}

// ====================================================================
// kDirect（直投）：不入队、不占槽位、不过门
// ====================================================================

namespace {

/// @brief 链序观测：层进来时记账（层号必须严格递增）。
struct SOrderObs
{
    SOrderObs() : nNext(0), nViolations(0)
    {}

    std::atomic<int> nNext;        ///< 下一个应当到来的层号。
    std::atomic<int> nViolations;  ///< 层序违例次数（应为 0）。

    /// @brief 记账一层（层号与预期不符 = 违例）。
    ///
    /// @param nLayer 本层号（从 0 开始）。
    void Enter(int nLayer)
    {
        if (nNext.fetch_add(1) != nLayer)
        {
            nViolations.fetch_add(1);
        }
    }
};

}  // namespace

/// @brief `kDirect` 投递不过门：写任务占着门时，直投任务照样立即跑（与写者重叠）。
///
/// 构造是确定的：写者跑完自己的读写业务后**不马上退出**，而是等直投任务跑掉才放开
/// `bWriterHolds` —— 若直投任务真被门挡住（排队等槽位），它只能在写者退出之后才跑，
/// 于是「重叠」计数为 0，本用例失败。
TEST(AsyncRw_DirectPostBypassesGate)
{
    CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    SRwState state;
    std::atomic<bool> bWriterHolds(false);
    std::atomic<bool> bDirectRan(false);
    std::atomic<int> nDirectOverlap(0);

    std::shared_ptr<SRwCtx> spWriteCtx = std::make_shared<SRwCtx>();
    spWriteCtx->nId = 1;
    CPromise<SRwCtx> writer = exec.NewPromise(
        spWriteCtx,
        [&state, &bWriterHolds, &bDirectRan](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            bWriterHolds.store(true);
            RunWriteWork(&state);  // 正常写业务：独占进入模块
            // 保持门不放：直投任务若走门就会被卡在这里（500ms 后放弃 → 断言失败）。
            WaitUntil(
                [&bDirectRan]()
                {
                    return bDirectRan.load();
                },
                500);
            bWriterHolds.store(false);
            return CPromiseResult::Resolve();
        },
        TaskKind::kWrite);

    ASSERT_TRUE(WaitUntil(
        [&bWriterHolds]()
        {
            return bWriterHolds.load();
        },
        1000));

    // 直投任务：写者正占着门 —— 它不过门，所以在另一条线程上立刻跑起来。
    ASSERT_TRUE(exec.Post(TaskKind::kDirect,
        [&bWriterHolds, &bDirectRan, &nDirectOverlap]()
        {
            if (bWriterHolds.load())
            {
                nDirectOverlap.fetch_add(1);  // 与写者重叠 = 确实绕过了门
            }
            bDirectRan.store(true);
        }));

    ASSERT_TRUE(writer.Await().IsFulfilled());
    exec.Stop();

    ASSERT_TRUE(bDirectRan.load());
    ASSERT_EQ(nDirectOverlap.load(), 1);     // 直投任务在写者退出之前就跑完了
    ASSERT_EQ(state.nViolations.load(), 0);  // 直投任务不碰模块状态：写者互斥没被破坏
}

/// @brief `kDirect` 链：首层与后续层都不过门 —— 写者占着门时整条直投链照样跑完。
TEST(AsyncRw_DirectChainBypassesGate)
{
    CAsyncExecutor exec(3);
    ASSERT_TRUE(exec.Start());

    SRwState state;
    std::atomic<bool> bWriterHolds(false);
    std::atomic<int> nDirectLayers(0);
    std::atomic<int> nDirectOverlap(0);

    std::shared_ptr<SRwCtx> spWriteCtx = std::make_shared<SRwCtx>();
    spWriteCtx->nId = 1;
    CPromise<SRwCtx> writer = exec.NewPromise(
        spWriteCtx,
        [&state, &bWriterHolds, &nDirectLayers](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            bWriterHolds.store(true);
            RunWriteWork(&state);
            // 保持门不放：直投链两层都跑完才退出。
            WaitUntil(
                [&nDirectLayers]()
                {
                    return nDirectLayers.load() >= 2;
                },
                500);
            bWriterHolds.store(false);
            return CPromiseResult::Resolve();
        },
        TaskKind::kWrite);

    ASSERT_TRUE(WaitUntil(
        [&bWriterHolds]()
        {
            return bWriterHolds.load();
        },
        1000));

    std::shared_ptr<SRwCtx> spDirectCtx = std::make_shared<SRwCtx>();
    spDirectCtx->nId = 2;
    CPromise<SRwCtx> direct = exec.NewPromise(
        spDirectCtx,
        [&bWriterHolds, &nDirectLayers, &nDirectOverlap](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            if (bWriterHolds.load())
            {
                nDirectOverlap.fetch_add(1);
            }
            nDirectLayers.fetch_add(1);
            return CPromiseResult::Resolve();
        },
        TaskKind::kDirect);
    direct = direct.Then(
        [&bWriterHolds, &nDirectLayers, &nDirectOverlap](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            if (bWriterHolds.load())
            {
                nDirectOverlap.fetch_add(1);
            }
            nDirectLayers.fetch_add(1);
            return CPromiseResult::Resolve();
        },
        TaskKind::kDirect, ASYNC_LOC);

    ASSERT_TRUE(direct.Await().IsFulfilled());
    ASSERT_TRUE(writer.Await().IsFulfilled());
    exec.Stop();

    ASSERT_EQ(nDirectLayers.load(), 2);
    ASSERT_EQ(nDirectOverlap.load(), 2);  // 两层都在写者持有门期间跑掉（整链不过门）
}

/// @brief 同一条链里写 / 直投 / 读混排：按链序执行（直投层不改变链的先后关系）。
TEST(AsyncRw_MixedKindsChainKeepsOrder)
{
    CAsyncExecutor exec(3);
    ASSERT_TRUE(exec.Start());

    SOrderObs obs;
    SRwState state;
    std::atomic<int> nDone(0);

    std::shared_ptr<SRwCtx> spCtx = std::make_shared<SRwCtx>();
    spCtx->nId = 1;
    CPromise<SRwCtx> chain = exec.NewPromise(
        spCtx,
        [&obs, &state, &nDone](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            obs.Enter(0);
            RunWriteWork(&state);  // 写层：独占进入
            nDone.fetch_add(1);
            return CPromiseResult::Resolve();
        },
        TaskKind::kWrite);
    chain = chain.Then(
        [&obs, &nDone](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            obs.Enter(1);
            nDone.fetch_add(1);  // 直投层：不过门（写层还在手里也照跑）
            return CPromiseResult::Resolve();
        },
        TaskKind::kDirect, ASYNC_LOC);
    chain = chain.Then(
        [&obs, &state, &nDone](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            obs.Enter(2);
            RunReadWork(&state);  // 读层：可与别的读并发
            nDone.fetch_add(1);
            return CPromiseResult::Resolve();
        },
        TaskKind::kRead, ASYNC_LOC);

    ASSERT_TRUE(chain.Await().IsFulfilled());
    exec.Stop();

    ASSERT_EQ(nDone.load(), 3);
    ASSERT_EQ(obs.nNext.load(), 3);
    ASSERT_EQ(obs.nViolations.load(), 0);    // 链序没被直投层打乱
    ASSERT_EQ(state.nViolations.load(), 0);  // 写层独占、读层不发违例
}

/// @brief 组合器的聚合层**不过门**：写者占着门时，空集合 `WhenAll` 照样当场落定。
///
/// 聚合层是「框架簿记层」（只被 settle、不跑业务代码）→ 用 `kKindBookkeeping`（不过门）。
/// 若它去占一个槽位（例如按写者排队），下面 `AwaitFor` 只等 300ms 就会超时 → 本用例失败。
TEST(AsyncRw_GatherLayerBypassesGate)
{
    CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    SRwState state;
    std::atomic<bool> bWriterHolds(false);
    std::atomic<bool> bRelease(false);

    std::shared_ptr<SRwCtx> spWriteCtx = std::make_shared<SRwCtx>();
    spWriteCtx->nId = 1;
    CPromise<SRwCtx> writer = exec.NewPromise(
        spWriteCtx,
        [&state, &bWriterHolds, &bRelease](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            bWriterHolds.store(true);
            RunWriteWork(&state);  // 写者：独占进入模块
            WaitUntil(
                [&bRelease]()
                {
                    return bRelease.load();
                },
                2000);  // 保持门直到主线程验完
            bWriterHolds.store(false);
            return CPromiseResult::Resolve();
        },
        TaskKind::kWrite);

    ASSERT_TRUE(WaitUntil(
        [&bWriterHolds]()
        {
            return bWriterHolds.load();
        },
        1000));

    // 空集合：聚合层是簿记层 → 不排队、当场收口（all / allSettled 视为成功）。
    ASSERT_TRUE(exec.WhenAll(spWriteCtx).AwaitFor(300).IsFulfilled());
    ASSERT_TRUE(exec.WhenAllSettled(spWriteCtx).AwaitFor(300).IsFulfilled());

    bRelease.store(true);
    ASSERT_TRUE(writer.Await().IsFulfilled());
    exec.Stop();

    ASSERT_EQ(state.nViolations.load(), 0);
}

#if defined(ASYNC_DEBUG_TRACE)
/// @brief 直投层在 trace 里看得到：`CLayerInfo::eKind == kDirect`，单层描述里带「直」。
TEST(AsyncRw_DirectKindVisibleInTrace)
{
    CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<SRwCtx> spCtx = std::make_shared<SRwCtx>();
    spCtx->nId = 1;
    std::atomic<int> nDirectLayers(0);
    std::string strDescribe;

    CPromise<SRwCtx> chain = exec.NewPromise(
        spCtx,
        [&nDirectLayers](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            const common::async::CLayerInfo* pInfo = common::async::CurrentLayer();
            if (pInfo != NULL && pInfo->eKind == TaskKind::kDirect)
            {
                nDirectLayers.fetch_add(1);
            }
            return CPromiseResult::Resolve();
        },
        TaskKind::kDirect);
    chain = chain.Then(
        [&nDirectLayers, &strDescribe](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            const common::async::CLayerInfo* pInfo = common::async::CurrentLayer();
            if (pInfo != NULL && pInfo->eKind == TaskKind::kDirect)
            {
                nDirectLayers.fetch_add(1);
            }
            if (pInfo != NULL)
            {
                strDescribe = common::async::DescribeLayer(*pInfo);  // 排障视图：类别列打印「直」
            }
            return CPromiseResult::Resolve();
        },
        TaskKind::kDirect, ASYNC_LOC);

    ASSERT_TRUE(chain.Await().IsFulfilled());
    exec.Stop();

    ASSERT_EQ(nDirectLayers.load(), 2);                        // 两层都是直投
    ASSERT_TRUE(strDescribe.find("直") != std::string::npos);  // 描述里的类别列写「直」
}
#endif  // defined(ASYNC_DEBUG_TRACE)
