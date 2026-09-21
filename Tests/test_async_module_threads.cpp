/// @file test_async_module_threads.cpp
/// 「一个模块 = 一个执行器（多线程）」时，模块状态线程安全的验证。
///
/// 场景：模块自持 4 线程执行器（典型形态，见 docs/common/async-usage.md §8.1），模块状态是**普通成员**
/// （`std::map` + 几个计数器，**不加锁**），并发安全全部来自执行器的读写门。用例逐条钉住这套做法成立
/// 与不成立的地方：
///  - 多线程确实在并发：读链峰值 ≥ 2、观测到 ≥ 2 个工作线程（否则后面「状态自洽」只是串行假象）；
///  - 写任务互斥 ⇒ 「单个任务内」的读-改-写不丢更新：并发写后非原子的计数器仍然精确；
///  - 读任务只读 ⇒ 并发读不会读到撕裂状态：读层反复校验「地图 / 总和 / 加权和 / 操作数」自洽；
///  - 写不重叠、读写不重叠（多线程下依然成立）；
///  - **门只保证「单个任务」互斥，不保证「整条链」独占**：写链的层与层之间会让位给别的任务 ——
///    用例用手动标记确定性地证明这一点（这条正是 ServerExample 要靠乐观锁兜住的地方）；
///  - 跨层流程的正确写法：乐观锁「读版本 → 带版本写 → 冲突重读重试」能收敛、不丢更新；
///  - 停止：混合流量中 `Stop()` 不破坏状态自洽、不再跑新层；
///  - 多客户端混合流量压力：全部落定、零违例、终局状态精确。
///
/// 说明：`ASSERT_*` 只在主测试线程执行；工作线程与客户端线程只更新原子状态 / 上下文（每链一份）。
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "Async/ReadWriteGate.h"
#include "TestFramework.h"

namespace {

using common::async::CAsyncExecutor;
using common::async::CPromise;
using common::async::CPromiseResult;
using common::async::TaskKind;

/// 模块执行器线程数（「一个模块 = 一个执行器（多线程）」）。
const int kModuleThreads = 4;
/// 模块状态的分桶数（桶 = 操作 id % kBuckets）。
const int kBuckets = 8;
/// 单个任务里的模拟耗时（毫秒）：放大并发窗口，让「本该互斥却没互斥」暴露出来。
const int kStepMs = 1;
/// 标记等待的超时（毫秒；超时即视为「等不到」，用例按失败处理）。
const int kMarkWaitMs = 3000;

// ====================================================================
// 观测与同步工具
// ====================================================================

/// @brief 读写观测（工作线程只写原子；主线程等待后断言）。
struct SModuleObs
{
    SModuleObs()
        : nReaders(0),
          nWriters(0),
          nPeakReaders(0),
          nPeakWriters(0),
          nViolations(0),
          nInvariantErrors(0),
          nReadTasks(0),
          nWriteTasks(0),
          nConflicts(0)
    {}

    std::atomic<int> nReaders;          ///< 当前读任务数。
    std::atomic<int> nWriters;          ///< 当前写任务数（> 1 即违例）。
    std::atomic<int> nPeakReaders;      ///< 读并发峰值（多线程模块上应 ≥ 2）。
    std::atomic<int> nPeakWriters;      ///< 写并发峰值（应恒为 1）。
    std::atomic<int> nViolations;       ///< 违例次数（读写重叠 / 写者重叠；应为 0）。
    std::atomic<int> nInvariantErrors;  ///< 读到自洽性被破坏的次数（撕裂读；应为 0）。
    std::atomic<int> nReadTasks;        ///< 已完成的读任务数。
    std::atomic<int> nWriteTasks;       ///< 已完成的写任务数。
    std::atomic<int> nConflicts;        ///< 乐观锁冲突次数（观测用：是否发生取决于调度，不作断言）。

    /// @brief 抬高峰值。
    ///
    /// @param nPeak 峰值计数。
    /// @param nNow 当前值。
    static void RaisePeak(std::atomic<int>& nPeak, int nNow)
    {
        int nMax = nPeak.load();
        while (nNow > nMax && !nPeak.compare_exchange_weak(nMax, nNow))
        {
        }
    }

    /// @brief 读任务进入：与写者同时在场 = 违例。
    void EnterRead()
    {
        if (nWriters.load() > 0)
        {
            nViolations.fetch_add(1);
        }
        RaisePeak(nPeakReaders, nReaders.fetch_add(1) + 1);
    }

    /// @brief 读任务离开。
    void LeaveRead()
    {
        nReaders.fetch_sub(1);
        nReadTasks.fetch_add(1);
    }

    /// @brief 写任务进入：与读者或其它写者同时在场 = 违例。
    void EnterWrite()
    {
        if (nReaders.load() > 0)
        {
            nViolations.fetch_add(1);
        }
        if (nWriters.fetch_add(1) + 1 > 1)
        {
            nViolations.fetch_add(1);
        }
        RaisePeak(nPeakWriters, nWriters.load());
    }

    /// @brief 写任务离开。
    void LeaveWrite()
    {
        nWriters.fetch_sub(1);
        nWriteTasks.fetch_add(1);
    }
};

/// @brief 一次性标记（跨链同步用：谁先到谁把 `bSet` 置真）。
struct SFlag
{
    SFlag() : bSet(false)
    {}

    std::atomic<bool> bSet;  ///< 是否已亮起。
};

/// @brief 限时等待标记亮起（轮询；超时返回 false —— 调用方按「等不到」处理，不会挂住）。
///
/// @param spFlag 待等待的标记。
/// @param nWaitMs 超时（毫秒）。
///
/// @return true 在超时前等到。
bool WaitFlag(const std::shared_ptr<SFlag>& spFlag, int nWaitMs)
{
    if (spFlag == nullptr)
    {
        return true;
    }
    const std::chrono::steady_clock::time_point tStart = std::chrono::steady_clock::now();
    while (!spFlag->bSet.load())
    {
        const long nElapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tStart).count();
        if (nElapsed >= nWaitMs)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

/// @brief 模拟耗时（拉长并发窗口）。
///
/// @param nMs 毫秒数。
void SleepMs(int nMs)
{
    if (nMs > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(nMs));
    }
}

// ====================================================================
// 被测模块：多线程执行器 + 不加锁的模块状态
// ====================================================================

/// @brief 模块上下文（每条链一份；各字段只被本链的层写，主线程在所有链落定后读）。
struct SModuleCtx
{
    int nOpId;                           ///< 操作 id（决定落在哪个桶）。
    int nRetry;                          ///< 乐观锁重试次数（用例 6）。
    long long nVersionRead;              ///< 读到的版本（乐观锁写回时期望值）。
    std::thread::id idLayer1;            ///< 第 1 层所在线程。
    std::thread::id idLayer2;            ///< 第 2 层所在线程。
    std::shared_ptr<SFlag> spOwnMark1;   ///< 本链第 1 层亮起的标记（用例 5）。
    std::shared_ptr<SFlag> spPeerMark1;  ///< 对端第 1 层的标记（用例 5）。
    std::shared_ptr<SFlag> spOwnMark2;   ///< 本链第 2 层亮起的标记（用例 5）。
    std::shared_ptr<SFlag> spPeerMark2;  ///< 对端第 2 层的标记（用例 5）。
    bool bWaitedPeer1;                   ///< 第 2 层是否等到了对端第 1 层（用例 5）。
    bool bWaitedPeer2;                   ///< 第 3 层是否等到了对端第 2 层（用例 5）。

    SModuleCtx() : nOpId(0), nRetry(0), nVersionRead(0), idLayer1(), idLayer2(), bWaitedPeer1(false), bWaitedPeer2(false)
    {}
};

/// @brief 假模块：自持多线程执行器；状态是普通成员（**不加锁**，靠读写门保证安全）。
///
/// 状态不变量（每个**写任务**结束时成立；读任务据此判定有没有撕裂读）：
///  - `m_nOps == m_nSum`（每次写既 +1 计数也 +1 总和）；
///  - `m_nSum == Σ m_mapBuckets[i]`；
///  - `m_nWeighted == Σ (i + 1) * m_mapBuckets[i]`。
class CFakeStateModule
{
public:
    /// @brief 创建模块并启动执行器。
    ///
    /// @param nThreads 模块执行器线程数（多线程 = 并发读 + 互斥写）。
    /// @param spObs 观测点（可空）。
    explicit CFakeStateModule(int nThreads, const std::shared_ptr<SModuleObs>& spObs)
        : m_exec("state", static_cast<size_t>(nThreads)),
          m_nSum(0),
          m_nWeighted(0),
          m_nOps(0),
          m_nValue(0),
          m_nVersion(0),
          m_spObs(spObs)
    {
        m_exec.Start();
    }

    ~CFakeStateModule()
    {
        m_exec.Stop();
    }

    CFakeStateModule(const CFakeStateModule&) = delete;
    CFakeStateModule& operator=(const CFakeStateModule&) = delete;

    /// @brief 状态快照（只在「没有任务在跑」时调用：用例在全部链落定 / `Stop()` 之后读）。
    struct SStats
    {
        std::size_t nEntries;  ///< 桶数（非空桶个数）。
        long long nSum;        ///< 计数总和。
        long long nWeighted;   ///< 加权和。
        long long nOps;        ///< 写操作数。
        long long nValue;      ///< 用例 6 的值。
        long long nVersion;    ///< 用例 6 的版本。
    };

    /// @brief 取状态快照。
    ///
    /// @return 快照（无锁读：调用方保证此刻没有任务在跑）。
    SStats Snapshot() const
    {
        SStats stats;
        stats.nEntries = m_mapBuckets.size();
        stats.nSum = m_nSum;
        stats.nWeighted = m_nWeighted;
        stats.nOps = m_nOps;
        stats.nValue = m_nValue;
        stats.nVersion = m_nVersion;
        return stats;
    }

    /// @brief 由状态自己算不变量（读层调用；与三个计数器比对）。
    ///
    /// @return true 状态自洽。
    bool VerifyInvariants() const
    {
        long long nSum = 0;
        long long nWeighted = 0;
        for (std::map<int, long long>::const_iterator it = m_mapBuckets.begin(); it != m_mapBuckets.end(); ++it)
        {
            nSum += it->second;
            nWeighted += static_cast<long long>(it->first + 1) * it->second;
        }
        return m_nSum == nSum && m_nWeighted == nWeighted && m_nOps == m_nSum &&
               m_mapBuckets.size() <= static_cast<std::size_t>(kBuckets);
    }

    /// @brief 停止模块执行器（之后投递一律被拒绝）。
    void Stop()
    {
        m_exec.Stop();
    }

    // ---------------- 对外异步函数（读 / 写） ----------------

    /// @brief 写：单任务内把「桶计数 / 总和 / 加权和 / 操作数」一起改完（状态在任务边界恒自洽）。
    ///
    /// @param spCtx 操作上下文（nOpId 决定桶）。
    ///
    /// @return 本链的 promise（单层写链，类别 = 默认写）。
    CPromise<SModuleCtx> AddAsync(const std::shared_ptr<SModuleCtx>& spCtx)
    {
        return m_exec.NewPromise(
            spCtx,
            [this](const std::shared_ptr<SModuleCtx>& spCtxSelf)
            {
                return StepAdd(spCtxSelf);
            },
            common::async::TaskKind::kWrite, ASYNC_LOC);
    }

    /// @brief 读：校验模块状态自洽（撕裂读检测）。
    ///
    /// @param spCtx 操作上下文（只用 idLayer1 记线程）。
    ///
    /// @return 本链的 promise（单层读链，类别 = 读；可与其它读并发）。
    CPromise<SModuleCtx> CheckAsync(const std::shared_ptr<SModuleCtx>& spCtx)
    {
        return m_exec.NewPromise(
            spCtx,
            [this](const std::shared_ptr<SModuleCtx>& spCtxSelf)
            {
                return StepCheck(spCtxSelf);
            },
            TaskKind::kRead, ASYNC_LOC);
    }

    /// @brief 写：三层链（层内互斥、层间让位；只做互斥观测与标记，不改状态）。
    ///
    /// @param spCtx 操作上下文（标记与等待结果写回本上下文）。
    ///
    /// @return 本链的 promise（三层写链）。
    CPromise<SModuleCtx> WriteChainAsync(const std::shared_ptr<SModuleCtx>& spCtx)
    {
        return m_exec
            .NewPromise(
                spCtx,
                [this](const std::shared_ptr<SModuleCtx>& spCtxSelf)
                {
                    return StepChain1(spCtxSelf);
                },
                common::async::TaskKind::kWrite, ASYNC_LOC)
            .Then(
                [this](const std::shared_ptr<SModuleCtx>& spCtxSelf)
                {
                    return StepChain2(spCtxSelf);
                },
                common::async::TaskKind::kWrite, ASYNC_LOC)
            .Then(
                [this](const std::shared_ptr<SModuleCtx>& spCtxSelf)
                {
                    return StepChain3(spCtxSelf);
                },
                common::async::TaskKind::kWrite, ASYNC_LOC);
    }

    /// @brief 读：读快照与版本（乐观锁的「读」半边）。
    ///
    /// @param spCtx 操作上下文（结果写入 nVersionRead）。
    ///
    /// @return 本链的 promise（单层读链）。
    CPromise<SModuleCtx> ReadVersionAsync(const std::shared_ptr<SModuleCtx>& spCtx)
    {
        return m_exec.NewPromise(
            spCtx,
            [this](const std::shared_ptr<SModuleCtx>& spCtxSelf)
            {
                return StepReadVersion(spCtxSelf);
            },
            TaskKind::kRead, ASYNC_LOC);
    }

    /// @brief 写：带版本校验的写（冲突即拒绝 `版本冲突`，由调用方重试）。
    ///
    /// @param spCtx 操作上下文（nVersionRead 为期望版本）。
    ///
    /// @return 本链的 promise（单层写链）。
    CPromise<SModuleCtx> TryIncrementAsync(const std::shared_ptr<SModuleCtx>& spCtx)
    {
        return m_exec.NewPromise(
            spCtx,
            [this](const std::shared_ptr<SModuleCtx>& spCtxSelf)
            {
                return StepTryIncrement(spCtxSelf);
            },
            common::async::TaskKind::kWrite, ASYNC_LOC);
    }

private:
    // ---------------- 层（处理器） ----------------

    /// 写层：桶计数 + 三个总和字段一起改（都在同一个任务里，状态在任务边界恒自洽）。
    CPromiseResult StepAdd(const std::shared_ptr<SModuleCtx>& spCtx)
    {
        m_spObs->EnterWrite();
        SleepMs(kStepMs);
        const int nBucket = spCtx->nOpId % kBuckets;
        ++m_mapBuckets[nBucket];
        ++m_nSum;
        m_nWeighted += static_cast<long long>(nBucket + 1);
        ++m_nOps;
        m_spObs->LeaveWrite();
        return CPromiseResult::Resolve();
    }

    /// 读层：只读状态（不写任何成员）——「读任务不得修改模块状态」是本机制唯一的规约。
    CPromiseResult StepCheck(const std::shared_ptr<SModuleCtx>& spCtx)
    {
        m_spObs->EnterRead();
        spCtx->idLayer1 = std::this_thread::get_id();
        SleepMs(kStepMs);
        if (!VerifyInvariants())
        {
            m_spObs->nInvariantErrors.fetch_add(1);
        }
        m_spObs->LeaveRead();
        return CPromiseResult::Resolve();
    }

    /// 写链第 1 层：亮起本链标记（对端的第 2 层会等它）。
    CPromiseResult StepChain1(const std::shared_ptr<SModuleCtx>& spCtx)
    {
        m_spObs->EnterWrite();
        spCtx->idLayer1 = std::this_thread::get_id();
        SleepMs(kStepMs);
        if (spCtx->spOwnMark1 != nullptr)
        {
            spCtx->spOwnMark1->bSet.store(true);
        }
        m_spObs->LeaveWrite();
        return CPromiseResult::Resolve();
    }

    /// 写链第 2 层：等对端第 1 层 → 亮起本链第 2 层标记。
    ///
    /// 等得到 = 上一条写链第 1 层与第 2 层之间真的让位了（层间不独占）。
    CPromiseResult StepChain2(const std::shared_ptr<SModuleCtx>& spCtx)
    {
        m_spObs->EnterWrite();
        spCtx->idLayer2 = std::this_thread::get_id();
        spCtx->bWaitedPeer1 = WaitFlag(spCtx->spPeerMark1, kMarkWaitMs);
        SleepMs(kStepMs);
        if (spCtx->spOwnMark2 != nullptr)
        {
            spCtx->spOwnMark2->bSet.store(true);
        }
        m_spObs->LeaveWrite();
        return CPromiseResult::Resolve();
    }

    /// 写链第 3 层：等对端第 2 层（再验证一次层间让位）。
    CPromiseResult StepChain3(const std::shared_ptr<SModuleCtx>& spCtx)
    {
        m_spObs->EnterWrite();
        spCtx->bWaitedPeer2 = WaitFlag(spCtx->spPeerMark2, kMarkWaitMs);
        SleepMs(kStepMs);
        m_spObs->LeaveWrite();
        return CPromiseResult::Resolve();
    }

    /// 读层：读版本（乐观锁）。
    CPromiseResult StepReadVersion(const std::shared_ptr<SModuleCtx>& spCtx)
    {
        m_spObs->EnterRead();
        spCtx->nVersionRead = m_nVersion;
        m_spObs->LeaveRead();
        return CPromiseResult::Resolve();
    }

    /// 写层：版本一致才改（不一致 → 拒绝，交给调用方重读重试）。
    CPromiseResult StepTryIncrement(const std::shared_ptr<SModuleCtx>& spCtx)
    {
        m_spObs->EnterWrite();
        SleepMs(kStepMs);
        if (spCtx->nVersionRead != m_nVersion)
        {
            m_spObs->nConflicts.fetch_add(1);
            m_spObs->LeaveWrite();
            return CPromiseResult::Reject(std::runtime_error("版本冲突"));
        }
        ++m_nValue;
        ++m_nVersion;
        m_spObs->LeaveWrite();
        return CPromiseResult::Resolve();
    }

    CAsyncExecutor m_exec;                  ///< 模块执行器（多线程 + 读写门）。
    std::map<int, long long> m_mapBuckets;  ///< 状态①：桶 → 计数（**非原子**，只被写任务改）。
    long long m_nSum;                       ///< 状态②：计数总和（不变量 m_nOps == m_nSum）。
    long long m_nWeighted;                  ///< 状态③：加权和（Σ (桶+1) * 计数）。
    long long m_nOps;                       ///< 状态④：写操作数。
    long long m_nValue;                     ///< 状态⑤：乐观锁的值。
    long long m_nVersion;                   ///< 状态⑥：乐观锁的版本。
    std::shared_ptr<SModuleObs> m_spObs;    ///< 观测点（只读；内部字段全是原子）。
};

// ====================================================================
// 客户端工具：外部线程按接口调用模块的异步函数（像真实调用方那样）
// ====================================================================

/// @brief 造一个上下文。
///
/// @param nOpId 操作 id。
///
/// @return 上下文。
std::shared_ptr<SModuleCtx> MakeCtx(int nOpId)
{
    std::shared_ptr<SModuleCtx> spCtx = std::make_shared<SModuleCtx>();
    spCtx->nOpId = nOpId;
    return spCtx;
}

// ====================================================================
// 用例
// ====================================================================

/// @brief 前置事实：多线程模块上「读真的在并发、写真的只有一个」。
///
/// 若这条不成立（例如退化成了单线程），后面几条「状态自洽」的用例会在串行假象下通过，结论就不可信。
TEST(ModuleThreads_MultiThreadConcurrencyObserved)
{
    std::shared_ptr<SModuleObs> spObs = std::make_shared<SModuleObs>();
    CFakeStateModule module(kModuleThreads, spObs);

    const int kReadChains = 8;
    const int kWriteChains = 8;
    std::vector<CPromise<SModuleCtx> > vecReads;
    std::vector<CPromise<SModuleCtx> > vecWrites;
    std::vector<std::shared_ptr<SModuleCtx> > vecCtx;
    for (int i = 0; i < kReadChains; ++i)
    {
        std::shared_ptr<SModuleCtx> spCtx = MakeCtx(i);
        vecCtx.push_back(spCtx);
        vecReads.push_back(module.CheckAsync(spCtx));
    }
    for (int i = 0; i < kWriteChains; ++i)
    {
        std::shared_ptr<SModuleCtx> spCtx = MakeCtx(i);
        vecCtx.push_back(spCtx);
        vecWrites.push_back(module.AddAsync(spCtx));
    }
    for (size_t i = 0; i < vecReads.size(); ++i)
    {
        ASSERT_TRUE(vecReads[i].Await().IsFulfilled());
    }
    for (size_t i = 0; i < vecWrites.size(); ++i)
    {
        ASSERT_TRUE(vecWrites[i].Await().IsFulfilled());
    }

    // 并发事实：读峰值 ≥ 2（多线程真的用上了）；写峰值恒为 1（互斥）。
    ASSERT_TRUE(spObs->nPeakReaders.load() >= 2);
    ASSERT_EQ(spObs->nPeakWriters.load(), 1);
    ASSERT_EQ(spObs->nViolations.load(), 0);

    // 观测到的工作线程 ≥ 2（模块自己的线程池在跑，不是调用方线程）。
    std::set<std::thread::id> setModuleThreads;
    for (size_t i = 0; i < vecCtx.size(); ++i)
    {
        setModuleThreads.insert(vecCtx[i]->idLayer1);
    }
    ASSERT_TRUE(setModuleThreads.size() >= 2);
    ASSERT_TRUE(setModuleThreads.count(std::this_thread::get_id()) == 0);
}

/// @brief 并发写不丢更新：多客户端线程同时压「单任务读-改-写」，终局计数精确。
///
/// 模块状态是**非原子**的普通成员 —— 若写任务没有互斥，`++m_mapBuckets[...]` / `++m_nSum` 必然丢写，
/// 终局计数就会小于下发次数（这条断言就是「丢更新」的探针）。
TEST(ModuleThreads_ConcurrentWritesKeepExactCount)
{
    std::shared_ptr<SModuleObs> spObs = std::make_shared<SModuleObs>();
    CFakeStateModule module(kModuleThreads, spObs);

    const int kClients = 6;
    const int kPerClient = 20;
    std::vector<std::thread> vecClients;
    std::atomic<int> nFulfilled(0);
    for (int c = 0; c < kClients; ++c)
    {
        vecClients.push_back(std::thread(
            [c, kPerClient, &module, &nFulfilled]()
            {
                for (int i = 0; i < kPerClient; ++i)
                {
                    std::shared_ptr<SModuleCtx> spCtx = MakeCtx(c * 100 + i);
                    if (module.AddAsync(spCtx).Await().IsFulfilled())
                    {
                        nFulfilled.fetch_add(1);
                    }
                }
            }));
    }
    for (size_t i = 0; i < vecClients.size(); ++i)
    {
        vecClients[i].join();
    }

    const int nExpected = kClients * kPerClient;
    ASSERT_EQ(nFulfilled.load(), nExpected);
    ASSERT_EQ(spObs->nWriteTasks.load(), nExpected);
    ASSERT_EQ(spObs->nViolations.load(), 0);
    ASSERT_EQ(spObs->nPeakWriters.load(), 1);

    // 终局状态：计数精确 + 不变量自洽。
    CFakeStateModule::SStats stats = module.Snapshot();
    ASSERT_EQ(stats.nOps, nExpected);
    ASSERT_EQ(stats.nSum, nExpected);
    ASSERT_TRUE(stats.nWeighted > 0);
    ASSERT_TRUE(module.VerifyInvariants());
}

/// @brief 并发读不会读到撕裂状态：写与读同时进行，读层反复校验不变量。
///
/// 读任务只读、写任务独占 ⇒ 读永远落在「某个写任务完成之后」的完整状态上（不会看到「桶计数加了、
/// 总和还没加」这种中间态）。撕裂就会把 nInvariantErrors 抬高。
TEST(ModuleThreads_ReadersNeverSeeTornState)
{
    std::shared_ptr<SModuleObs> spObs = std::make_shared<SModuleObs>();
    CFakeStateModule module(kModuleThreads, spObs);

    const int kWriterClients = 2;
    const int kWriterPerClient = 30;
    const int kReaderClients = 4;
    const int kReaderPerClient = 40;
    std::atomic<int> nViolationSeen(0);

    std::vector<std::thread> vecClients;
    for (int c = 0; c < kWriterClients; ++c)
    {
        vecClients.push_back(std::thread(
            [c, kWriterPerClient, &module, &nViolationSeen]()
            {
                for (int i = 0; i < kWriterPerClient; ++i)
                {
                    std::shared_ptr<SModuleCtx> spCtx = MakeCtx(c * 1000 + i);
                    if (!module.AddAsync(spCtx).Await().IsFulfilled())
                    {
                        nViolationSeen.fetch_add(1);
                    }
                }
            }));
    }
    for (int c = 0; c < kReaderClients; ++c)
    {
        vecClients.push_back(std::thread(
            [c, kReaderPerClient, &module, &nViolationSeen]()
            {
                for (int i = 0; i < kReaderPerClient; ++i)
                {
                    std::shared_ptr<SModuleCtx> spCtx = MakeCtx(c * 1000 + i);
                    if (!module.CheckAsync(spCtx).Await().IsFulfilled())
                    {
                        nViolationSeen.fetch_add(1);
                    }
                }
            }));
    }
    for (size_t i = 0; i < vecClients.size(); ++i)
    {
        vecClients[i].join();
    }

    ASSERT_EQ(nViolationSeen.load(), 0);
    ASSERT_EQ(spObs->nInvariantErrors.load(), 0);  // 撕裂读探针
    ASSERT_EQ(spObs->nViolations.load(), 0);
    ASSERT_TRUE(spObs->nInvariantErrors.load() + spObs->nViolations.load() == 0);
    ASSERT_EQ(spObs->nWriteTasks.load(), kWriterClients * kWriterPerClient);
    ASSERT_TRUE(spObs->nReadTasks.load() >= kReaderClients * kReaderPerClient);
    ASSERT_TRUE(module.VerifyInvariants());
}

/// @brief 三层写链：层与层**不重叠**（写者峰值恒为 1），读写也不重叠。
TEST(ModuleThreads_WriteChainLayersDoNotOverlap)
{
    std::shared_ptr<SModuleObs> spObs = std::make_shared<SModuleObs>();
    CFakeStateModule module(kModuleThreads, spObs);

    const int kClients = 4;
    const int kPerClient = 6;
    std::vector<std::thread> vecClients;
    for (int c = 0; c < kClients; ++c)
    {
        vecClients.push_back(std::thread(
            [c, kPerClient, &module]()
            {
                for (int i = 0; i < kPerClient; ++i)
                {
                    module.WriteChainAsync(MakeCtx(c * 100 + i)).Await();
                }
            }));
    }
    for (size_t i = 0; i < vecClients.size(); ++i)
    {
        vecClients[i].join();
    }

    ASSERT_EQ(spObs->nViolations.load(), 0);   // 层内互斥 + 读写不重叠
    ASSERT_EQ(spObs->nPeakWriters.load(), 1);  // 任何时刻最多一个写任务在跑
    ASSERT_EQ(spObs->nWriteTasks.load(), kClients * kPerClient * 3);
}

/// @brief 语义事实：门只保证「单个任务」互斥，**不保证整条写链独占** —— 层间会让位给别的任务。
///
/// 构造是确定性的：两条三层写链互相等对方的标记 ——
///  - 若「整条写链独占」成立，A 的第 2 层会一直等到超时（B 的第 1 层进不来）→ `bWaitedPeer1` 为假 → 用例失败；
///  - 实际语义下 A 的第 1 层一结束就轮到 B 的第 1 层，标记随即亮起 → 两边都等得到 → 用例通过。
///
/// 这条正是「跨层的写流程不是原子的」的证据：需要整段读改写不被插队时，要么把读改写放进**同一个任务**，
/// 要么用乐观锁（见下一条用例）。
TEST(ModuleThreads_WriteChainYieldsBetweenLayers)
{
    std::shared_ptr<SModuleObs> spObs = std::make_shared<SModuleObs>();
    CFakeStateModule module(kModuleThreads, spObs);

    std::shared_ptr<SFlag> spMarkA1 = std::make_shared<SFlag>();
    std::shared_ptr<SFlag> spMarkB1 = std::make_shared<SFlag>();
    std::shared_ptr<SFlag> spMarkA2 = std::make_shared<SFlag>();
    std::shared_ptr<SFlag> spMarkB2 = std::make_shared<SFlag>();

    std::shared_ptr<SModuleCtx> spCtxA = MakeCtx(1);
    spCtxA->spOwnMark1 = spMarkA1;
    spCtxA->spPeerMark1 = spMarkB1;
    spCtxA->spOwnMark2 = spMarkA2;
    spCtxA->spPeerMark2 = spMarkB2;

    std::shared_ptr<SModuleCtx> spCtxB = MakeCtx(2);
    spCtxB->spOwnMark1 = spMarkB1;
    spCtxB->spPeerMark1 = spMarkA1;
    spCtxB->spOwnMark2 = spMarkB2;
    spCtxB->spPeerMark2 = spMarkA2;

    CPromise<SModuleCtx> promiseA = module.WriteChainAsync(spCtxA);
    CPromise<SModuleCtx> promiseB = module.WriteChainAsync(spCtxB);
    ASSERT_TRUE(promiseA.Await().IsFulfilled());
    ASSERT_TRUE(promiseB.Await().IsFulfilled());

    ASSERT_TRUE(spCtxA->bWaitedPeer1);        // A 的第 2 层等到了 B 的第 1 层 ⇒ 层间让位
    ASSERT_TRUE(spCtxB->bWaitedPeer1);        // B 的第 2 层等到了 A 的第 1 层
    ASSERT_TRUE(spCtxA->bWaitedPeer2);        // A 的第 3 层等到了 B 的第 2 层
    ASSERT_EQ(spObs->nViolations.load(), 0);  // 让位归让位，层内互斥照旧
    ASSERT_EQ(spObs->nPeakWriters.load(), 1);
}

/// @brief 跨层流程的正确写法：乐观锁「读版本 → 带版本写 → 冲突重读重试」不丢更新。
///
/// 客户端线程按接口调用（读链 + 写链都是模块内的任务），冲突时重试到最后一致。
TEST(ModuleThreads_OptimisticLockKeepsUpdatesExact)
{
    std::shared_ptr<SModuleObs> spObs = std::make_shared<SModuleObs>();
    CFakeStateModule module(kModuleThreads, spObs);

    const int kClients = 4;
    const int kPerClient = 20;
    std::atomic<int> nFailed(0);
    std::vector<std::thread> vecClients;
    for (int c = 0; c < kClients; ++c)
    {
        vecClients.push_back(std::thread(
            [c, kPerClient, &module, &nFailed]()
            {
                for (int i = 0; i < kPerClient; ++i)
                {
                    // 读版本 → 带版本写；冲突就重读重试（限次，避免真出 bug 时死循环）。
                    for (int nAttempt = 0; nAttempt < 32; ++nAttempt)
                    {
                        std::shared_ptr<SModuleCtx> spCtx = MakeCtx(c * 100 + i);
                        if (!module.ReadVersionAsync(spCtx).Await().IsFulfilled())
                        {
                            nFailed.fetch_add(1);
                            break;
                        }
                        CPromiseResult result = module.TryIncrementAsync(spCtx).Await();
                        if (result.IsFulfilled())
                        {
                            break;
                        }
                        if (nAttempt == 31)
                        {
                            nFailed.fetch_add(1);
                        }
                    }
                }
            }));
    }
    for (size_t i = 0; i < vecClients.size(); ++i)
    {
        vecClients[i].join();
    }

    ASSERT_EQ(nFailed.load(), 0);
    CFakeStateModule::SStats stats = module.Snapshot();
    ASSERT_EQ(stats.nValue, kClients * kPerClient);  // 不丢更新（逐次读改写最终账对得上）
    ASSERT_EQ(stats.nVersion, kClients * kPerClient);
    ASSERT_EQ(spObs->nViolations.load(), 0);
}

/// @brief 混合流量中 `Stop()`：状态自洽、结果要么兑现要么「执行器已停」、停止后不再跑新层。
TEST(ModuleThreads_StopUnderLoadKeepsStateConsistent)
{
    std::shared_ptr<SModuleObs> spObs = std::make_shared<SModuleObs>();
    CFakeStateModule module(kModuleThreads, spObs);

    const int kClients = 4;
    const int kPerClient = 40;
    std::atomic<int> nFulfilled(0);
    std::atomic<int> nStopped(0);
    std::atomic<int> nOther(0);

    std::vector<std::thread> vecClients;
    for (int c = 0; c < kClients; ++c)
    {
        vecClients.push_back(std::thread(
            [c, kPerClient, &module, &nFulfilled, &nStopped, &nOther]()
            {
                for (int i = 0; i < kPerClient; ++i)
                {
                    std::shared_ptr<SModuleCtx> spCtx = MakeCtx(c * 1000 + i);
                    const bool bRead = ((i & 1) == 0);
                    CPromiseResult result = bRead ? module.CheckAsync(spCtx).Await() : module.AddAsync(spCtx).Await();
                    if (result.IsFulfilled())
                    {
                        nFulfilled.fetch_add(1);
                    }
                    else if (result.Message() == "执行器已停")
                    {
                        nStopped.fetch_add(1);
                    }
                    else
                    {
                        nOther.fetch_add(1);
                    }
                }
            }));
    }

    // 让流量先跑起来，再半路停：已接受的任务跑完（关门 → 等排空 → 停池）。
    SleepMs(20);
    module.Stop();
    const int nWritesAtStop = spObs->nWriteTasks.load();
    module.Stop();  // 幂等：重复 Stop 不应出问题

    for (size_t i = 0; i < vecClients.size(); ++i)
    {
        vecClients[i].join();
    }

    ASSERT_EQ(nOther.load(), 0);  // 只允许「兑现」或「执行器已停」
    ASSERT_TRUE(nFulfilled.load() > 0);
    ASSERT_EQ(spObs->nViolations.load(), 0);
    ASSERT_EQ(spObs->nInvariantErrors.load(), 0);
    ASSERT_EQ(spObs->nWriteTasks.load(), nWritesAtStop);  // 停止后不再跑新层（写任务数不再增长）
    ASSERT_TRUE(module.VerifyInvariants());               // 终局状态自洽
}

/// @brief 多客户端混合流量压力：零违例、零撕裂读、终局账目精确。
TEST(ModuleThreads_StressMixedTraffic)
{
    std::shared_ptr<SModuleObs> spObs = std::make_shared<SModuleObs>();
    CFakeStateModule module(kModuleThreads, spObs);

    const int kClients = 6;
    const int kPerClient = 30;
    std::atomic<int> nFulfilled(0);
    std::atomic<int> nWrites(0);
    std::atomic<int> nReads(0);

    std::vector<std::thread> vecClients;
    for (int c = 0; c < kClients; ++c)
    {
        vecClients.push_back(std::thread(
            [c, kPerClient, &module, &nFulfilled, &nWrites, &nReads]()
            {
                for (int i = 0; i < kPerClient; ++i)
                {
                    std::shared_ptr<SModuleCtx> spCtx = MakeCtx(c * 1000 + i);
                    const bool bRead = ((i % 3) == 0);
                    const bool bOk =
                        bRead ? module.CheckAsync(spCtx).Await().IsFulfilled() : module.AddAsync(spCtx).Await().IsFulfilled();
                    if (bOk)
                    {
                        nFulfilled.fetch_add(1);
                        if (bRead)
                        {
                            nReads.fetch_add(1);
                        }
                        else
                        {
                            nWrites.fetch_add(1);
                        }
                    }
                }
            }));
    }
    for (size_t i = 0; i < vecClients.size(); ++i)
    {
        vecClients[i].join();
    }

    ASSERT_EQ(nFulfilled.load(), kClients * kPerClient);
    ASSERT_EQ(spObs->nViolations.load(), 0);
    ASSERT_EQ(spObs->nInvariantErrors.load(), 0);
    ASSERT_EQ(spObs->nWriteTasks.load(), nWrites.load());
    ASSERT_TRUE(spObs->nReadTasks.load() >= nReads.load());

    CFakeStateModule::SStats stats = module.Snapshot();
    ASSERT_EQ(stats.nOps, nWrites.load());
    ASSERT_EQ(stats.nSum, nWrites.load());
    ASSERT_TRUE(module.VerifyInvariants());
}

}  // namespace
