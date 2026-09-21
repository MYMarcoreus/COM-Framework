/// @file test_async_rw.cpp
/// 执行器 × 读写门（模块内「读并发 / 写独占」）的集成测试。
///
/// 正确性验证点：
///  - `Post(kWrite)`（写）与写链：与模块内其它任务互斥，写者唯一，读写不重叠；
///  - `Post(kRead)` 与读链（`NewPromise(..., TaskKind::kRead)`）：读之间可并发进入；
///  - 混合读写：读不越过写、写等到读者排空（无违例）；
///  - 逐层类别：同一链的层可读 / 写混排，每层按自己的类别过门；
///  - `kDirect`（直投）：不过门 —— 与在读 / 写任务并发，且不占槽位；
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
#include "Async/Promise.h"
#include "Async/ReadWriteGate.h"
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
        vecChains.push_back(exec.NewPromise(spCtx,
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
    common::async::CPromise<CInlineCtx> tail = exec.NewPromise(spCtx,
        [spCtx](const std::shared_ptr<CInlineCtx>& /*spCtx*/)
        {
            spCtx->idFirst = std::this_thread::get_id();
            spCtx->nSteps += 1;
            return CPromiseResult::Resolve();
        }, TaskKind::kWrite);
    for (int i = 1; i < kLayers; ++i)
    {
        tail = tail.Then(
            [spCtx](const std::shared_ptr<CInlineCtx>& /*spCtx*/)
            {
                spCtx->idLast = std::this_thread::get_id();
                spCtx->nSteps += 1;
                return CPromiseResult::Resolve();
            }, TaskKind::kWrite);
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
    CPromise<SRwCtx> chain = exec.NewPromise(spCtx,
        [&exec](const std::shared_ptr<SRwCtx>& /*spCtx*/) -> CPromiseResult
        {
            while (!exec.IsStopped())
            {
                std::this_thread::yield();
            }
            return CPromiseResult::Resolve();
        }, TaskKind::kWrite);
    chain = chain.Then(
        [](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            return CPromiseResult::Resolve();
        }, TaskKind::kWrite);

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
    CPromise<SRwCtx> chain = exec.NewPromise(spChainCtx,
        [&obs, &bWriteLayerRunning](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            bWriteLayerRunning.store(true);  // 让主线程知道写层已占住门
            std::this_thread::sleep_for(std::chrono::milliseconds(kWorkMs));
            obs.EnterWrite();
            return CPromiseResult::Resolve();
        }, TaskKind::kWrite);

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

    CPromise<SRwCtx> chain = exec.NewPromise(spCtx,
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
    ASSERT_TRUE(exec.Post(
        TaskKind::kDirect,
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

    ASSERT_EQ(nDirectLayers.load(), 2);                              // 两层都是直投
    ASSERT_TRUE(strDescribe.find("直") != std::string::npos);        // 描述里的类别列写「直」
}
#endif  // defined(ASYNC_DEBUG_TRACE)
