/// @file test_async_rw.cpp
/// 执行器 × 读写门（模块内「读并发 / 写独占」）的集成测试。
///
/// 正确性验证点：
///  - `Post(kWrite)`（写）与默认写链：与模块内其它任务互斥，写者唯一，读写不重叠；
///  - `Post(kRead)` 与读链（`NewPromise(..., TaskKind::kRead)`）：读之间可并发进入；
///  - 混合读写：读不越过写、写等到读者排空（无违例）；
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

/// @brief 默认类别是「写」：多条默认链（`NewPromise` 不传类别）在同一执行器上互斥。
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
            }));
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
            common::async::CSourceLoc(), TaskKind::kRead));
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
        });
    for (int i = 1; i < kLayers; ++i)
    {
        tail = tail.Then(
            [spCtx](const std::shared_ptr<CInlineCtx>& /*spCtx*/)
            {
                spCtx->idLast = std::this_thread::get_id();
                spCtx->nSteps += 1;
                return CPromiseResult::Resolve();
            });
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
        });
    chain = chain.Then(
        [](const std::shared_ptr<SRwCtx>& /*spCtx*/)
        {
            return CPromiseResult::Resolve();
        });

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
