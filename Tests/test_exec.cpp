/// @file test_exec.cpp
/// Exec 并发框架单元测试：全局线程池 + 模块级读写调度 + 业务流程回调栈。
///
/// 正确性验证点（不变式均按「模块」独立统计，因为不同模块允许并发）：
///  - 读任务可多线程并发进入（峰值并发读 > 1，且不超过模块上限）；
///  - 写任务独占（同一模块内至多一个写线程；读/写互斥无违例）；
///  - 全部子任务最终完成，业务流程回调栈恰好回放一次；
///  - 回调栈按 LIFO 顺序触发，且晚于全部子任务完成；
///  - 多模块链式（跨模块 / 同模块重入）嵌套调用无死锁、计数精确。
///
/// 说明：断言只允许在主测试线程执行；工作线程仅更新原子状态，
/// 测试结束后由主线程断言（避免子线程抛异常导致进程终止）。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "TestFramework.h"

#include "Exec/BusinessFlow.h"
#include "Exec/CallbackStack.h"
#include "Exec/GlobalDispatcher.h"
#include "Exec/ModuleScheduler.h"
#include "Thread/ThreadPool.h"

namespace {

using sc::CBusinessFlow;
using sc::CCallbackStack;
using sc::CGlobalDispatcher;
using sc::CModuleScheduler;
using common::thread::CThreadPool;

// 读/写子任务在模块内执行的持续时间（毫秒）：放大并发窗口便于观测。
const int kWorkMs = 2;

/// @brief 单个模块的读写观测状态（工作线程只写原子，主线程等待后断言）。
struct SModuleState
{
    SModuleState()
        : nActiveReaders(0), nActiveWriters(0),
          nPeakReaders(0), nPeakWriters(0), nViolations(0)
    {
    }

    std::atomic<int> nActiveReaders; // 本模块当前读子任务中的线程数
    std::atomic<int> nActiveWriters; // 本模块当前写子任务中的线程数
    std::atomic<int> nPeakReaders;   // 本模块观测到的最大并发读
    std::atomic<int> nPeakWriters;   // 本模块观测到的最大并发写
    std::atomic<long> nViolations;   // 本模块读写互斥违例次数（应为 0）
};

/// @brief 全局测试状态（跨模块的完成计数）。
struct STestState
{
    STestState() : nReadDone(0), nWriteDone(0), nFlowDone(0)
    {
    }

    std::atomic<long> nReadDone;  // 所有模块读子任务完成总数
    std::atomic<long> nWriteDone; // 所有模块写子任务完成总数
    std::atomic<long> nFlowDone;  // 流程完成数（回调栈回放次数）
};

/// @brief 读业务（进入本模块）：进入校验无写者 → 并发计数并记录峰值 → 短耗时 → 退出。
void RunReadWork(SModuleState* pModState, STestState* pGlobal)
{
    if (pModState->nActiveWriters.load() > 0)
    {
        pModState->nViolations.fetch_add(1); // 本模块读与写重叠 = 违例
    }
    const int nNow = pModState->nActiveReaders.fetch_add(1) + 1;
    int nPeak = pModState->nPeakReaders.load();
    while (nPeak < nNow && !pModState->nPeakReaders.compare_exchange_weak(nPeak, nNow))
    {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kWorkMs));
    pModState->nActiveReaders.fetch_sub(1);
    pGlobal->nReadDone.fetch_add(1);
}

/// @brief 写业务（进入本模块）：进入校验无读者/无其他写者 → 独占计数 → 短耗时 → 退出。
void RunWriteWork(SModuleState* pModState, STestState* pGlobal)
{
    if (pModState->nActiveReaders.load() > 0)
    {
        pModState->nViolations.fetch_add(1); // 本模块写与读重叠 = 违例
    }
    const int nNow = pModState->nActiveWriters.fetch_add(1) + 1;
    if (nNow > 1)
    {
        pModState->nViolations.fetch_add(1); // 本模块写者必须唯一
    }
    int nPeak = pModState->nPeakWriters.load();
    while (nPeak < nNow && !pModState->nPeakWriters.compare_exchange_weak(nPeak, nNow))
    {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kWorkMs));
    pModState->nActiveWriters.fetch_sub(1);
    pGlobal->nWriteDone.fetch_add(1);
}

/// @brief 测试业务模块基类：持有模块级调度器 + 本模块读写观测状态。
///        对应服务器中的一个业务模块（模块内子任务由调度器把关）。
class CTestModuleBase
{
public:
    explicit CTestModuleBase(CThreadPool* pPool, size_t nMaxReaders)
        : m_scheduler(pPool, nMaxReaders)
    {
    }

    CModuleScheduler* Scheduler() { return &m_scheduler; }
    SModuleState& ModState() { return m_state; }

protected:
    CModuleScheduler m_scheduler; // 模块级读写调度器。
    SModuleState m_state;         // 本模块读写观测状态。
};

/// @brief 读模块：只提交读子任务（多线程并发进入）。
class CReaderModule : public CTestModuleBase
{
public:
    explicit CReaderModule(CThreadPool* pPool, size_t nMaxReaders)
        : CTestModuleBase(pPool, nMaxReaders)
    {
    }

    bool SubmitRead(const std::shared_ptr<CBusinessFlow>& spFlow, STestState* pGlobal)
    {
        return spFlow->SubmitTask(Scheduler(), CModuleScheduler::ETaskKind::kRead,
            [this, pGlobal]() { RunReadWork(&m_state, pGlobal); });
    }
};

/// @brief 写模块：只提交写子任务（独占进入）。
class CWriterModule : public CTestModuleBase
{
public:
    explicit CWriterModule(CThreadPool* pPool)
        : CTestModuleBase(pPool, 0)
    {
    }

    bool SubmitWrite(const std::shared_ptr<CBusinessFlow>& spFlow, STestState* pGlobal)
    {
        return spFlow->SubmitTask(Scheduler(), CModuleScheduler::ETaskKind::kWrite,
            [this, pGlobal]() { RunWriteWork(&m_state, pGlobal); });
    }
};

/// @brief 读写混合模块：读并发 + 写独占。
class CRwModule : public CTestModuleBase
{
public:
    explicit CRwModule(CThreadPool* pPool, size_t nMaxReaders)
        : CTestModuleBase(pPool, nMaxReaders)
    {
    }

    bool SubmitRead(const std::shared_ptr<CBusinessFlow>& spFlow, STestState* pGlobal)
    {
        return spFlow->SubmitTask(Scheduler(), CModuleScheduler::ETaskKind::kRead,
            [this, pGlobal]() { RunReadWork(&m_state, pGlobal); });
    }

    bool SubmitWrite(const std::shared_ptr<CBusinessFlow>& spFlow, STestState* pGlobal)
    {
        return spFlow->SubmitTask(Scheduler(), CModuleScheduler::ETaskKind::kWrite,
            [this, pGlobal]() { RunWriteWork(&m_state, pGlobal); });
    }
};

/// @brief 模拟业务模块：持有调度器 + 本模块读写观测状态。
///        收到业务请求后，在流程主体线程（单线程）内处理并扇出多个读/写子任务。
class CSimModule
{
public:
    explicit CSimModule(CThreadPool* pPool, size_t nMaxReaders)
        : m_scheduler(pPool, nMaxReaders), m_state()
    {
    }

    CModuleScheduler* Scheduler() { return &m_scheduler; }
    SModuleState& ModState() { return m_state; }

    /// @brief 处理一次业务请求（在调用线程 = 流程主体线程内执行）。
    ///        产生多个读/写子任务：本模块 2 读 + 1 写、依赖模块 1 读、通知模块 1 写。
    ///
    /// @param spFlow 所属业务流程（子任务自动计数）。
    /// @param pReadPeer 依赖模块（读）。
    /// @param pWritePeer 通知模块（写）。
    /// @param pGlobal 全局完成计数。
    void HandleRequest(const std::shared_ptr<CBusinessFlow>& spFlow,
                       CSimModule* pReadPeer, CSimModule* pWritePeer, STestState* pGlobal)
    {
        std::shared_ptr<CBusinessFlow> sp = spFlow;
        // 本模块：读缓存（并发）
        sp->SubmitTask(Scheduler(), CModuleScheduler::ETaskKind::kRead,
            [this, pGlobal]() { RunReadWork(&m_state, pGlobal); });
        sp->SubmitTask(Scheduler(), CModuleScheduler::ETaskKind::kRead,
            [this, pGlobal]() { RunReadWork(&m_state, pGlobal); });
        // 本模块：更新数据（写独占）
        sp->SubmitTask(Scheduler(), CModuleScheduler::ETaskKind::kWrite,
            [this, pGlobal]() { RunWriteWork(&m_state, pGlobal); });
        // 依赖模块：查询（读）
        sp->SubmitTask(pReadPeer->Scheduler(), CModuleScheduler::ETaskKind::kRead,
            [pReadPeer, pGlobal]() { RunReadWork(&pReadPeer->ModState(), pGlobal); });
        // 通知模块：写
        sp->SubmitTask(pWritePeer->Scheduler(), CModuleScheduler::ETaskKind::kWrite,
            [pWritePeer, pGlobal]() { RunWriteWork(&pWritePeer->ModState(), pGlobal); });
    }

private:
    CModuleScheduler m_scheduler;
    SModuleState m_state;
};

/// @brief 轮询等待条件满足（超时返回 false）。
bool WaitUntil(const std::function<bool()>& fnCond, int nTimeoutMs)
{
    const std::chrono::steady_clock::time_point tStart = std::chrono::steady_clock::now();
    while (!fnCond())
    {
        const long nElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tStart).count();
        if (nElapsed >= nTimeoutMs)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

} // namespace

/// @brief 回调栈基础语义：LIFO 出栈触发。
TEST(Exec_CallbackStack_Lifo)
{
    CCallbackStack stack;
    std::vector<std::string> vecOrder;
    stack.Push([&vecOrder]() { vecOrder.push_back("first"); });
    stack.Push([&vecOrder]() { vecOrder.push_back("second"); });
    stack.Push([&vecOrder]() { vecOrder.push_back("third"); });
    stack.RunAll();
    ASSERT_TRUE(stack.Empty());
    ASSERT_TRUE(vecOrder.size() == 3);
    ASSERT_TRUE(vecOrder[0] == "third");  // 后压先触发
    ASSERT_TRUE(vecOrder[1] == "second");
    ASSERT_TRUE(vecOrder[2] == "first");
}

/// @brief 流程无子任务：主体结束后回调栈回放恰好一次。
TEST(Exec_Flow_Complete_NoSubtask)
{
    CThreadPool pool(2);
    ASSERT_TRUE(pool.Start());
    CGlobalDispatcher dispatcher(&pool);

    std::atomic<long> nCb(0);
    const bool bDispatch = dispatcher.Dispatch(
        [&nCb](const std::shared_ptr<CBusinessFlow>& spFlow)
        {
            spFlow->Callbacks().Push([&nCb]() { nCb.fetch_add(1); });
            spFlow->Callbacks().Push([&nCb]() { nCb.fetch_add(1); });
        });
    ASSERT_TRUE(bDispatch);

    const bool bDone = WaitUntil([&nCb]() { return nCb.load() == 2; }, 3000);
    pool.Stop();
    ASSERT_TRUE(bDone);
}

/// @brief 流程带子任务：回调栈晚于全部子任务完成之后回放。
TEST(Exec_Flow_Complete_WithSubtask)
{
    CThreadPool pool(2);
    ASSERT_TRUE(pool.Start());
    CGlobalDispatcher dispatcher(&pool);
    CWriterModule writer(&pool);
    dispatcher.RegisterScheduler("writer", writer.Scheduler());

    std::atomic<int> nWorkDone(0); // 子任务内置位
    std::atomic<long> nCbAfterWork(0);
    const bool bDispatch = dispatcher.Dispatch(
        [&writer, &nWorkDone, &nCbAfterWork](const std::shared_ptr<CBusinessFlow>& spFlow)
        {
            spFlow->SubmitTask(writer.Scheduler(), CModuleScheduler::ETaskKind::kWrite,
                [&nWorkDone]()
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    nWorkDone.store(1);
                });
            // 回调应在子任务完成后触发：仅当 nWorkDone==1 才计数
            spFlow->Callbacks().Push([&nWorkDone, &nCbAfterWork]()
            {
                if (nWorkDone.load() == 1) { nCbAfterWork.fetch_add(1); }
            });
        });
    ASSERT_TRUE(bDispatch);

    const bool bDone = WaitUntil([&nCbAfterWork]() { return nCbAfterWork.load() == 1; }, 3000);
    pool.Stop();
    ASSERT_TRUE(bDone);
}

/// @brief 读并发：多读子任务可同时进入，峰值 ≤ 模块上限，且 > 1。
TEST(Exec_Rw_ReadConcurrent)
{
    const int kThreads = 8;
    const int kMaxReaders = 4;
    const int kFlows = 32;
    CThreadPool pool(kThreads);
    ASSERT_TRUE(pool.Start());
    CGlobalDispatcher dispatcher(&pool);
    CReaderModule reader(&pool, kMaxReaders);
    dispatcher.RegisterScheduler("reader", reader.Scheduler());

    STestState state;
    for (int i = 0; i < kFlows; ++i)
    {
        ASSERT_TRUE(dispatcher.Dispatch(
            [&reader, &state](const std::shared_ptr<CBusinessFlow>& spFlow)
            {
                reader.SubmitRead(spFlow, &state);
                spFlow->Callbacks().Push([&state]() { state.nFlowDone.fetch_add(1); });
            }));
    }

    const bool bDone = WaitUntil(
        [&state]() { return state.nFlowDone.load() == kFlows; }, 10000);
    pool.Stop();
    ASSERT_TRUE(bDone);
    ASSERT_TRUE(state.nReadDone.load() == kFlows);           // 全部读完成
    ASSERT_TRUE(reader.ModState().nPeakReaders.load() >= 2); // 确实并发进入
    ASSERT_TRUE(reader.ModState().nPeakReaders.load() <= kMaxReaders); // 未超上限
    ASSERT_TRUE(reader.ModState().nViolations.load() == 0); // 无读写重叠
}

/// @brief 写独占：多写子任务串行，峰值写 = 1，无违例。
TEST(Exec_Rw_WriteExclusive)
{
    const int kThreads = 8;
    const int kFlows = 32;
    CThreadPool pool(kThreads);
    ASSERT_TRUE(pool.Start());
    CGlobalDispatcher dispatcher(&pool);
    CWriterModule writer(&pool);
    dispatcher.RegisterScheduler("writer", writer.Scheduler());

    STestState state;
    for (int i = 0; i < kFlows; ++i)
    {
        ASSERT_TRUE(dispatcher.Dispatch(
            [&writer, &state](const std::shared_ptr<CBusinessFlow>& spFlow)
            {
                writer.SubmitWrite(spFlow, &state);
                spFlow->Callbacks().Push([&state]() { state.nFlowDone.fetch_add(1); });
            }));
    }

    const bool bDone = WaitUntil(
        [&state]() { return state.nFlowDone.load() == kFlows; }, 10000);
    pool.Stop();
    ASSERT_TRUE(bDone);
    ASSERT_TRUE(state.nWriteDone.load() == kFlows);      // 全部写完成
    ASSERT_TRUE(writer.ModState().nPeakWriters.load() == 1); // 写者唯一
    ASSERT_TRUE(writer.ModState().nViolations.load() == 0);  // 无读写重叠
}

/// @brief 同模块重入：读子任务内再提交同模块写子任务（非阻塞 → 无死锁）。
TEST(Exec_Rw_ReentrantSameModule)
{
    const int kThreads = 4;
    const int kFlows = 20;
    CThreadPool pool(kThreads);
    ASSERT_TRUE(pool.Start());
    CGlobalDispatcher dispatcher(&pool);
    CRwModule rw(&pool, 2);
    dispatcher.RegisterScheduler("rw", rw.Scheduler());

    STestState state;
    for (int i = 0; i < kFlows; ++i)
    {
        ASSERT_TRUE(dispatcher.Dispatch(
            [&rw, &state](const std::shared_ptr<CBusinessFlow>& spFlow)
            {
                std::shared_ptr<CBusinessFlow> sp = spFlow; // 按值供嵌套捕获保活
                sp->SubmitTask(rw.Scheduler(), CModuleScheduler::ETaskKind::kRead,
                    [sp, &rw, &state]()
                    {
                        RunReadWork(&rw.ModState(), &state);
                        // 读子任务内再提交同模块写子任务（写需等读退出）
                        sp->SubmitTask(rw.Scheduler(), CModuleScheduler::ETaskKind::kWrite,
                            [&rw, &state]() { RunWriteWork(&rw.ModState(), &state); });
                    });
                sp->Callbacks().Push([&state]() { state.nFlowDone.fetch_add(1); });
            }));
    }

    const bool bDone = WaitUntil(
        [&state]() { return state.nFlowDone.load() == kFlows; }, 15000);
    pool.Stop();
    ASSERT_TRUE(bDone);
    ASSERT_TRUE(state.nReadDone.load() == kFlows);
    ASSERT_TRUE(state.nWriteDone.load() == kFlows);
    ASSERT_TRUE(rw.ModState().nViolations.load() == 0);
}

/// @brief 多模块链式调用：A 读 → B 写 → C 写（跨模块嵌套）+ B 并发读。
///        验证跨模块调度正确性、无死锁、计数精确（不变式按模块独立统计）。
TEST(Exec_Rw_MultiModuleChain)
{
    const int kThreads = 16;
    const int kFlows = 200;
    CThreadPool pool(kThreads);
    ASSERT_TRUE(pool.Start());
    CGlobalDispatcher dispatcher(&pool);
    CReaderModule readerA(&pool, 4); // 模块 A：读
    CRwModule     rwB(&pool, 3);     // 模块 B：读写混合
    CWriterModule writerC(&pool);    // 模块 C：写
    dispatcher.RegisterScheduler("A", readerA.Scheduler());
    dispatcher.RegisterScheduler("B", rwB.Scheduler());
    dispatcher.RegisterScheduler("C", writerC.Scheduler());

    STestState state;
    for (int i = 0; i < kFlows; ++i)
    {
        ASSERT_TRUE(dispatcher.Dispatch(
            [&readerA, &rwB, &writerC, &dispatcher, &state](
                const std::shared_ptr<CBusinessFlow>& spFlow)
            {
                std::shared_ptr<CBusinessFlow> sp = spFlow; // 按值供嵌套捕获保活
                // 模块 A：读子任务
                sp->SubmitTask(readerA.Scheduler(), CModuleScheduler::ETaskKind::kRead,
                    [sp, &readerA, &rwB, &writerC, &dispatcher, &state]()
                    {
                        RunReadWork(&readerA.ModState(), &state);
                        // 链式调用模块 B：写子任务（经注册表查找）
                        CModuleScheduler* pB = dispatcher.FindScheduler("B");
                        sp->SubmitTask(pB, CModuleScheduler::ETaskKind::kWrite,
                            [sp, &rwB, &writerC, &dispatcher, &state]()
                            {
                                RunWriteWork(&rwB.ModState(), &state);
                                // 链式调用模块 C：写子任务
                                CModuleScheduler* pC = dispatcher.FindScheduler("C");
                                sp->SubmitTask(pC, CModuleScheduler::ETaskKind::kWrite,
                                    [&writerC, &state]()
                                    {
                                        RunWriteWork(&writerC.ModState(), &state);
                                    });
                            });
                    });
                // 模块 B：读子任务（与 B 的写并发/排队）
                sp->SubmitTask(rwB.Scheduler(), CModuleScheduler::ETaskKind::kRead,
                    [&rwB, &state]() { RunReadWork(&rwB.ModState(), &state); });
                // 流程收尾：完成后回放
                sp->Callbacks().Push([&state]() { state.nFlowDone.fetch_add(1); });
            }));
    }

    const bool bDone = WaitUntil(
        [&state]() { return state.nFlowDone.load() == kFlows; }, 30000);
    pool.Stop();
    ASSERT_TRUE(bDone);
    // 每流程：A 读 + B 读 = 2 读；B 写 + C 写 = 2 写
    const long nExpectedRead = static_cast<long>(kFlows) * 2;
    const long nExpectedWrite = static_cast<long>(kFlows) * 2;
    ASSERT_TRUE(state.nReadDone.load() == nExpectedRead);
    ASSERT_TRUE(state.nWriteDone.load() == nExpectedWrite);
    // 各模块不变式：A 读确实并发；B 写唯一；无违例
    ASSERT_TRUE(readerA.ModState().nPeakReaders.load() >= 2);
    ASSERT_TRUE(rwB.ModState().nPeakWriters.load() == 1);
    ASSERT_TRUE(readerA.ModState().nViolations.load() == 0);
    ASSERT_TRUE(rwB.ModState().nViolations.load() == 0);
    ASSERT_TRUE(writerC.ModState().nViolations.load() == 0);
}

/// @brief 调度器排空：等待排空后空闲、计数精确。
TEST(Exec_Scheduler_DrainIdle)
{
    const int kThreads = 4;
    const int kFlows = 20;
    CThreadPool pool(kThreads);
    ASSERT_TRUE(pool.Start());
    CGlobalDispatcher dispatcher(&pool);
    CWriterModule writer(&pool);
    dispatcher.RegisterScheduler("writer", writer.Scheduler());

    STestState state;
    for (int i = 0; i < kFlows; ++i)
    {
        ASSERT_TRUE(dispatcher.Dispatch(
            [&writer, &state](const std::shared_ptr<CBusinessFlow>& spFlow)
            {
                writer.SubmitWrite(spFlow, &state);
            }));
    }

    const bool bDone = WaitUntil(
        [&state]() { return state.nWriteDone.load() == kFlows; }, 10000);
    pool.Stop();
    dispatcher.DrainAll();
    ASSERT_TRUE(bDone);
    ASSERT_TRUE(writer.Scheduler()->IsIdle());
    ASSERT_TRUE(state.nWriteDone.load() == kFlows);
}

/// @brief 压力：多模块混合大量流程（读/写交错），无死锁、计数精确。
TEST(Exec_Rw_Stress)
{
    const int kThreads = 16;
    const int kFlows = 2000;
    CThreadPool pool(kThreads);
    ASSERT_TRUE(pool.Start());
    CGlobalDispatcher dispatcher(&pool);
    CRwModule rwA(&pool, 4); // 模块 A：读写混合
    CRwModule rwB(&pool, 4); // 模块 B：读写混合
    dispatcher.RegisterScheduler("A", rwA.Scheduler());
    dispatcher.RegisterScheduler("B", rwB.Scheduler());

    STestState state;
    for (int i = 0; i < kFlows; ++i)
    {
        // 注意：i 必须按值捕获（流程异步执行，引用捕获循环变量会悬垂）
        ASSERT_TRUE(dispatcher.Dispatch(
            [&rwA, &rwB, &state, i](const std::shared_ptr<CBusinessFlow>& spFlow)
            {
                if ((i & 1) == 0)
                {
                    rwA.SubmitRead(spFlow, &state);
                    rwB.SubmitWrite(spFlow, &state);
                }
                else
                {
                    rwA.SubmitWrite(spFlow, &state);
                    rwB.SubmitRead(spFlow, &state);
                }
                spFlow->Callbacks().Push([&state]() { state.nFlowDone.fetch_add(1); });
            }));
    }

    const bool bDone = WaitUntil(
        [&state]() { return state.nFlowDone.load() == kFlows; }, 60000);
    pool.Stop();
    ASSERT_TRUE(bDone);
    // 每流程：1 读 + 1 写
    ASSERT_TRUE(state.nReadDone.load() == kFlows);
    ASSERT_TRUE(state.nWriteDone.load() == kFlows);
    ASSERT_TRUE(rwA.ModState().nPeakWriters.load() == 1); // 各模块写唯一
    ASSERT_TRUE(rwB.ModState().nPeakWriters.load() == 1);
    ASSERT_TRUE(rwA.ModState().nViolations.load() == 0);
    ASSERT_TRUE(rwB.ModState().nViolations.load() == 0);
}

/// @brief 高并发读：32 线程、读上限 16，验证读确实大规模并行且不超上限。
TEST(Exec_Rw_HighConcurrencyRead)
{
    const int kThreads = 32;
    const int kMaxReaders = 16;
    const int kFlows = 128;
    CThreadPool pool(kThreads);
    ASSERT_TRUE(pool.Start());
    CGlobalDispatcher dispatcher(&pool);
    CReaderModule reader(&pool, kMaxReaders);
    dispatcher.RegisterScheduler("reader", reader.Scheduler());

    STestState state;
    for (int i = 0; i < kFlows; ++i)
    {
        ASSERT_TRUE(dispatcher.Dispatch(
            [&reader, &state](const std::shared_ptr<CBusinessFlow>& spFlow)
            {
                reader.SubmitRead(spFlow, &state);
                spFlow->Callbacks().Push([&state]() { state.nFlowDone.fetch_add(1); });
            }));
    }

    const bool bDone = WaitUntil(
        [&state]() { return state.nFlowDone.load() == kFlows; }, 15000);
    pool.Stop();
    ASSERT_TRUE(bDone);
    ASSERT_TRUE(state.nReadDone.load() == kFlows);
    ASSERT_TRUE(reader.ModState().nPeakReaders.load() >= 8);       // 高并发确实发生
    ASSERT_TRUE(reader.ModState().nPeakReaders.load() <= kMaxReaders); // 不超上限
    ASSERT_TRUE(reader.ModState().nViolations.load() == 0);
}

/// @brief 多生产者并发投递：8 个线程同时 Dispatch，验证并发提交路径无竞争、
///        调度器在多生产 / 多线程并发进入下仍保持读写互斥与精确计数。
TEST(Exec_Rw_MultiProducerDispatch)
{
    const int kThreads = 32;
    const int kProducers = 8;
    const int kFlowsPerProducer = 500;
    const int kModules = 4;
    const int kFlows = kProducers * kFlowsPerProducer;

    CThreadPool pool(kThreads);
    ASSERT_TRUE(pool.Start());
    CGlobalDispatcher dispatcher(&pool);

    // 4 个读写混合模块（unique_ptr 持有：CModuleScheduler 含 mutex 不可拷贝）
    std::vector<std::unique_ptr<CRwModule>> vecModules;
    for (int m = 0; m < kModules; ++m)
    {
        std::unique_ptr<CRwModule> spMod(new CRwModule(&pool, 8));
        dispatcher.RegisterScheduler("M" + std::to_string(m), spMod->Scheduler());
        vecModules.push_back(std::move(spMod));
    }

    STestState state;
    std::atomic<long> nDispatchFail(0);

    // 8 个生产者线程并发投递业务请求
    std::vector<std::thread> vecProducers;
    for (int p = 0; p < kProducers; ++p)
    {
        vecProducers.push_back(std::thread([&, p]()
        {
            for (int i = 0; i < kFlowsPerProducer; ++i)
            {
                const int nReadMod = (p + i) % kModules;
                const int nWriteMod = (p + i + 1) % kModules;
                if (!dispatcher.Dispatch(
                        [&, nReadMod, nWriteMod](const std::shared_ptr<CBusinessFlow>& spFlow)
                        {
                            vecModules[nReadMod]->SubmitRead(spFlow, &state);
                            vecModules[nWriteMod]->SubmitWrite(spFlow, &state);
                            spFlow->Callbacks().Push([&state]() { state.nFlowDone.fetch_add(1); });
                        }))
                {
                    nDispatchFail.fetch_add(1);
                }
            }
        }));
    }
    for (std::vector<std::thread>::iterator it = vecProducers.begin();
         it != vecProducers.end(); ++it)
    {
        it->join();
    }

    const bool bDone = WaitUntil(
        [&state]() { return state.nFlowDone.load() == kFlows; }, 30000);
    pool.Stop();
    ASSERT_TRUE(bDone);
    ASSERT_TRUE(nDispatchFail.load() == 0); // 全部投递成功
    ASSERT_TRUE(state.nReadDone.load() == kFlows);
    ASSERT_TRUE(state.nWriteDone.load() == kFlows);
    for (int m = 0; m < kModules; ++m)
    {
        ASSERT_TRUE(vecModules[m]->ModState().nPeakWriters.load() == 1); // 各模块写唯一
        ASSERT_TRUE(vecModules[m]->ModState().nViolations.load() == 0);  // 无违例
    }
}

/// @brief 多模块高并发压力：64 线程、16 模块、8000 流程确定性散布读写。
///        逐模块验证：读不超上限、写唯一、无违例；全局计数精确。
TEST(Exec_Rw_ManyModulesStress)
{
    const int kThreads = 64;
    const int kModules = 16;
    const int kFlows = 8000;

    CThreadPool pool(kThreads);
    ASSERT_TRUE(pool.Start());
    CGlobalDispatcher dispatcher(&pool);

    // 16 个读写混合模块，每模块读上限 4..7（验证上限在高压下不被突破）
    std::vector<std::unique_ptr<CRwModule>> vecModules;
    std::vector<int> vecReadCaps;
    for (int m = 0; m < kModules; ++m)
    {
        const int nCap = 4 + (m % 4);
        std::unique_ptr<CRwModule> spMod(new CRwModule(&pool, nCap));
        dispatcher.RegisterScheduler("M" + std::to_string(m), spMod->Scheduler());
        vecReadCaps.push_back(nCap);
        vecModules.push_back(std::move(spMod));
    }

    STestState state;
    for (int i = 0; i < kFlows; ++i)
    {
        // 确定性散布到各模块（可复现）
        const int nReadMod = (i * 5 + 1) % kModules;
        const int nWriteMod = (i * 7 + 3) % kModules;
        ASSERT_TRUE(dispatcher.Dispatch(
            [&, nReadMod, nWriteMod](const std::shared_ptr<CBusinessFlow>& spFlow)
            {
                vecModules[nReadMod]->SubmitRead(spFlow, &state);
                vecModules[nWriteMod]->SubmitWrite(spFlow, &state);
                spFlow->Callbacks().Push([&state]() { state.nFlowDone.fetch_add(1); });
            }));
    }

    const bool bDone = WaitUntil(
        [&state]() { return state.nFlowDone.load() == kFlows; }, 60000);
    pool.Stop();
    ASSERT_TRUE(bDone);
    ASSERT_TRUE(state.nReadDone.load() == kFlows);
    ASSERT_TRUE(state.nWriteDone.load() == kFlows);
    for (int m = 0; m < kModules; ++m)
    {
        ASSERT_TRUE(vecModules[m]->ModState().nPeakReaders.load() <= vecReadCaps[m]); // 读不超上限
        ASSERT_TRUE(vecModules[m]->ModState().nPeakWriters.load() == 1);             // 写唯一
        ASSERT_TRUE(vecModules[m]->ModState().nViolations.load() == 0);              // 无违例
    }
}

/// @brief 顺序（公平 FIFO）：同流程「先读后写」——写不插队到先前排队的读前面。
///        用占位读占满唯一读槽位，使流程 F 的读被迫排队，随后提交写；
///        验证执行顺序仍为 读→写（旧写优先策略会得到 写→读）。
TEST(Exec_Rw_Order_ReadThenWrite)
{
    CThreadPool pool(4);
    ASSERT_TRUE(pool.Start());
    CGlobalDispatcher dispatcher(&pool);
    CRwModule rw(&pool, 1); // 唯一读槽位
    dispatcher.RegisterScheduler("rw", rw.Scheduler());

    std::mutex mutex;
    std::vector<int> vecOrder;
    std::atomic<int> nReady(0);   // 占位读已活跃
    std::atomic<long> nF1Done(0); // 流程 F 完成

    // 占位读 R0：占据唯一读槽位一段时间
    ASSERT_TRUE(dispatcher.Dispatch(
        [&rw, &nReady](const std::shared_ptr<CBusinessFlow>& spFlow)
        {
            spFlow->SubmitTask(rw.Scheduler(), CModuleScheduler::ETaskKind::kRead,
                [&nReady]()
                {
                    nReady.store(1);
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                });
        }));

    // 等占位读活跃后，提交流程 F：先读后写（读会因槽位满而排队）
    ASSERT_TRUE(WaitUntil([&nReady]() { return nReady.load() == 1; }, 3000));
    ASSERT_TRUE(dispatcher.Dispatch(
        [&rw, &mutex, &vecOrder, &nF1Done](const std::shared_ptr<CBusinessFlow>& spFlow)
        {
            std::shared_ptr<CBusinessFlow> sp = spFlow;
            sp->SubmitTask(rw.Scheduler(), CModuleScheduler::ETaskKind::kRead,
                [&mutex, &vecOrder]()
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    vecOrder.push_back(1); // 读
                });
            sp->SubmitTask(rw.Scheduler(), CModuleScheduler::ETaskKind::kWrite,
                [&mutex, &vecOrder]()
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    vecOrder.push_back(2); // 写
                });
            sp->Callbacks().Push([&nF1Done]() { nF1Done.fetch_add(1); });
        }));

    const bool bDone = WaitUntil([&nF1Done]() { return nF1Done.load() == 1; }, 3000);
    pool.Stop();
    ASSERT_TRUE(bDone);
    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_TRUE(vecOrder.size() == 2);
    ASSERT_TRUE(vecOrder[0] == 1); // 读在前
    ASSERT_TRUE(vecOrder[1] == 2); // 写在后（写不插队）
}

/// @brief 顺序（公平 FIFO）：同流程「先写后读」——读不越过先前提交的写。
TEST(Exec_Rw_Order_WriteThenRead)
{
    CThreadPool pool(4);
    ASSERT_TRUE(pool.Start());
    CGlobalDispatcher dispatcher(&pool);
    CRwModule rw(&pool, 1);
    dispatcher.RegisterScheduler("rw", rw.Scheduler());

    std::mutex mutex;
    std::vector<int> vecOrder;
    std::atomic<long> nDone(0);

    ASSERT_TRUE(dispatcher.Dispatch(
        [&rw, &mutex, &vecOrder, &nDone](const std::shared_ptr<CBusinessFlow>& spFlow)
        {
            spFlow->SubmitTask(rw.Scheduler(), CModuleScheduler::ETaskKind::kWrite,
                [&mutex, &vecOrder]()
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    vecOrder.push_back(1); // 写
                });
            spFlow->SubmitTask(rw.Scheduler(), CModuleScheduler::ETaskKind::kRead,
                [&mutex, &vecOrder]()
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    vecOrder.push_back(2); // 读
                });
            spFlow->Callbacks().Push([&nDone]() { nDone.fetch_add(1); });
        }));

    const bool bDone = WaitUntil([&nDone]() { return nDone.load() == 1; }, 3000);
    pool.Stop();
    ASSERT_TRUE(bDone);
    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_TRUE(vecOrder.size() == 2);
    ASSERT_TRUE(vecOrder[0] == 1); // 写在前
    ASSERT_TRUE(vecOrder[1] == 2); // 读在后（读不越过写）
}

/// @brief 顺序（公平 FIFO）：写者严格按提交顺序执行（写者 FIFO）。
TEST(Exec_Rw_Order_WriterFifo)
{
    CThreadPool pool(4);
    ASSERT_TRUE(pool.Start());
    CGlobalDispatcher dispatcher(&pool);
    CWriterModule writer(&pool);
    dispatcher.RegisterScheduler("writer", writer.Scheduler());

    std::mutex mutex;
    std::vector<int> vecOrder;
    std::atomic<long> nDone(0);
    const int kWriters = 8;

    ASSERT_TRUE(dispatcher.Dispatch(
        [&writer, &mutex, &vecOrder, &nDone](const std::shared_ptr<CBusinessFlow>& spFlow)
        {
            for (int i = 0; i < kWriters; ++i)
            {
                spFlow->SubmitTask(writer.Scheduler(), CModuleScheduler::ETaskKind::kWrite,
                    [&mutex, &vecOrder, i]()
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        vecOrder.push_back(i); // 记录执行顺序
                    });
            }
            spFlow->Callbacks().Push([&nDone]() { nDone.fetch_add(1); });
        }));

    const bool bDone = WaitUntil([&nDone]() { return nDone.load() == 1; }, 3000);
    pool.Stop();
    ASSERT_TRUE(bDone);
    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_TRUE(vecOrder.size() == static_cast<size_t>(kWriters));
    for (int i = 0; i < kWriters; ++i)
    {
        ASSERT_TRUE(vecOrder[static_cast<size_t>(i)] == i); // 严格按提交顺序
    }
}

/// @brief 业务负载模拟：多个模块持续收到业务请求，每次处理在单线程内
///        扇出多个读/写子任务（本模块读/写 + 依赖读 + 通知写）。
///        验证：无死锁、全部完成、各模块读写互斥零违例、写唯一、读不超上限、计数精确。
TEST(Exec_Sim_BusinessLoad)
{
    const int kThreads = 16;
    const int kModules = 4;
    const int kProducers = 4;             // 模拟 4 个请求来源（客户端/网络）
    const int kRequestsPerProducer = 400; // 每来源持续派发的业务请求数
    const long kTotalFlows = static_cast<long>(kProducers) * kRequestsPerProducer;

    CThreadPool pool(kThreads);
    ASSERT_TRUE(pool.Start());
    CGlobalDispatcher dispatcher(&pool);

    // 4 个业务模块（读上限 4~5），持续接收请求
    std::vector<std::unique_ptr<CSimModule>> vecModules;
    std::vector<int> vecReadCaps;
    for (int m = 0; m < kModules; ++m)
    {
        const int nCap = 4 + (m % 2);
        std::unique_ptr<CSimModule> spMod(new CSimModule(&pool, nCap));
        dispatcher.RegisterScheduler("M" + std::to_string(m), spMod->Scheduler());
        vecReadCaps.push_back(nCap);
        vecModules.push_back(std::move(spMod));
    }

    STestState state;
    std::atomic<long> nDispatchFail(0);

    // 生产者线程：持续向各模块派发业务请求（目标轮转，模拟持续到达）
    std::vector<std::thread> vecProducers;
    for (int p = 0; p < kProducers; ++p)
    {
        vecProducers.push_back(std::thread([&, p]()
        {
            for (int i = 0; i < kRequestsPerProducer; ++i)
            {
                const int nTarget = (p + i) % kModules;
                if (!dispatcher.Dispatch(
                        [&, nTarget](const std::shared_ptr<CBusinessFlow>& spFlow)
                        {
                            CSimModule* pSelf = vecModules[nTarget].get();
                            CSimModule* pReadPeer = vecModules[(nTarget + 1) % kModules].get();
                            CSimModule* pWritePeer = vecModules[(nTarget + 2) % kModules].get();
                            // 一次业务处理：单线程内扇出多个读/写子任务
                            pSelf->HandleRequest(spFlow, pReadPeer, pWritePeer, &state);
                            // 处理结束：全部子任务完成后回放
                            spFlow->Callbacks().Push([&state]() { state.nFlowDone.fetch_add(1); });
                        }))
                {
                    nDispatchFail.fetch_add(1);
                }
            }
        }));
    }
    for (std::vector<std::thread>::iterator it = vecProducers.begin();
         it != vecProducers.end(); ++it)
    {
        it->join();
    }

    const bool bDone = WaitUntil(
        [&state]() { return state.nFlowDone.load() == kTotalFlows; }, 60000);
    pool.Stop();
    ASSERT_TRUE(bDone);
    ASSERT_TRUE(nDispatchFail.load() == 0); // 全部投递成功
    // 每流程：3 读 + 2 写
    ASSERT_TRUE(state.nReadDone.load() == kTotalFlows * 3);
    ASSERT_TRUE(state.nWriteDone.load() == kTotalFlows * 2);
    for (int m = 0; m < kModules; ++m)
    {
        ASSERT_TRUE(vecModules[m]->ModState().nPeakReaders.load() <= vecReadCaps[m]); // 读不超上限
        ASSERT_TRUE(vecModules[m]->ModState().nPeakWriters.load() == 1);             // 写唯一
        ASSERT_TRUE(vecModules[m]->ModState().nViolations.load() == 0);              // 读写互斥
    }
}
