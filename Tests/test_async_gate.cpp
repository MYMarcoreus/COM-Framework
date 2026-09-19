/// @file test_async_gate.cpp
/// 读写门（CReadWriteGate）单元测试：模块内「读并发 / 写独占 / 公平 FIFO」的准入闸。
///
/// 正确性验证点（不变式均按「门」独立统计 —— 不同门（模块）之间允许并发）：
///  - 读任务可多线程并发进入（峰值 > 1，且不超过 nMaxReaders）；
///  - 写任务独占（同一门内至多一个写任务；读写互斥无违例）；
///  - 公平 FIFO：任务不越过先提交的任务（写不插队到先排队的读前，读不越过先排队的写，
///    写者严格按提交顺序）；
///  - 非阻塞重入：任务内再向本门提交任务不会死锁（进不了就排队）；
///  - 任务抛异常仍归还槽位（否则门会永久卡死）；线程池不可用 / 已关闭时不接受投递；
///  - Drain 排空后空闲，计数精确。
///
/// 说明：断言只允许在主测试线程执行；工作线程仅更新原子状态，
/// 测试结束后由主线程断言（避免子线程抛异常导致进程终止）。
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "Async/Diagnostics.h"
#include "Async/ReadWriteGate.h"
#include "TestFramework.h"
#include "Thread/ThreadPool.h"

namespace {

using common::async::CReadWriteGate;
using common::async::TaskKind;
using common::thread::CThreadPool;

// 读/写任务在门内执行的持续时间（毫秒）：放大并发窗口便于观测。
const int kWorkMs = 2;

/// @brief 单个门的读写观测状态（工作线程只写原子，主线程等待后断言）。
struct SGateState
{
    SGateState() : nActiveReaders(0), nActiveWriters(0), nPeakReaders(0), nPeakWriters(0), nViolations(0)
    {}

    std::atomic<int> nActiveReaders;  // 本门当前读任务中的线程数
    std::atomic<int> nActiveWriters;  // 本门当前写任务中的线程数
    std::atomic<int> nPeakReaders;    // 本门观测到的最大并发读
    std::atomic<int> nPeakWriters;    // 本门观测到的最大并发写
    std::atomic<int> nViolations;     // 本门读写互斥违例次数（应为 0）
};

/// @brief 读业务（进入本门）：先校验无写者 → 并发计数并记录峰值 → 短耗时 → 退出。
void RunReadWork(SGateState* pState)
{
    if (pState->nActiveWriters.load() > 0)
    {
        pState->nViolations.fetch_add(1);  // 本门读与写重叠 = 违例
    }
    const int nNow = pState->nActiveReaders.fetch_add(1) + 1;
    int nPeak = pState->nPeakReaders.load();
    while (nPeak < nNow && !pState->nPeakReaders.compare_exchange_weak(nPeak, nNow))
    {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kWorkMs));
    pState->nActiveReaders.fetch_sub(1);
}

/// @brief 写业务（进入本门）：先校验无读者/无其他写者 → 独占计数 → 短耗时 → 退出。
void RunWriteWork(SGateState* pState)
{
    if (pState->nActiveReaders.load() > 0)
    {
        pState->nViolations.fetch_add(1);  // 本门写与读重叠 = 违例
    }
    const int nNow = pState->nActiveWriters.fetch_add(1) + 1;
    if (nNow > 1)
    {
        pState->nViolations.fetch_add(1);  // 本门写者必须唯一
    }
    int nPeak = pState->nPeakWriters.load();
    while (nPeak < nNow && !pState->nPeakWriters.compare_exchange_weak(nPeak, nNow))
    {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kWorkMs));
    pState->nActiveWriters.fetch_sub(1);
}

/// @brief 构造一个读任务体（跑读业务 + 计入完成数）。
std::function<void()> MakeReadTask(SGateState* pState, std::atomic<int>* pnDone)
{
    return [pState, pnDone]()
    {
        RunReadWork(pState);
        pnDone->fetch_add(1);
    };
}

/// @brief 构造一个写任务体（跑写业务 + 计入完成数）。
std::function<void()> MakeWriteTask(SGateState* pState, std::atomic<int>* pnDone)
{
    return [pState, pnDone]()
    {
        RunWriteWork(pState);
        pnDone->fetch_add(1);
    };
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

}  // namespace

/// @brief 读并发：多读任务可同时进入，峰值 ≤ 读上限，且 > 1。
TEST(Gate_ReadConcurrent)
{
    const int kThreads = 8;
    const size_t kMaxReaders = 4;
    const int kTasks = 32;

    CThreadPool pool(kThreads);
    ASSERT_TRUE(pool.Start());
    CReadWriteGate gate(&pool, kMaxReaders);

    SGateState state;
    std::atomic<int> nDone(0);
    for (int i = 0; i < kTasks; ++i)
    {
        ASSERT_TRUE(gate.Submit(TaskKind::kRead, MakeReadTask(&state, &nDone)));
    }

    const bool bDone = WaitUntil(
        [&nDone]()
        {
            return nDone.load() == kTasks;
        },
        10000);
    gate.Close();
    gate.Drain();
    pool.Stop();

    ASSERT_TRUE(bDone);
    ASSERT_TRUE(state.nPeakReaders.load() >= 2);                              // 确实并发进入
    ASSERT_TRUE(state.nPeakReaders.load() <= static_cast<int>(kMaxReaders));  // 未超上限
    ASSERT_TRUE(state.nViolations.load() == 0);                               // 无读写重叠
    ASSERT_TRUE(gate.IsIdle());
}

/// @brief 写独占：多写任务串行，峰值写 = 1，无违例。
TEST(Gate_WriteExclusive)
{
    const int kThreads = 8;
    const int kTasks = 32;

    CThreadPool pool(kThreads);
    ASSERT_TRUE(pool.Start());
    CReadWriteGate gate(&pool);

    SGateState state;
    std::atomic<int> nDone(0);
    for (int i = 0; i < kTasks; ++i)
    {
        ASSERT_TRUE(gate.Submit(TaskKind::kWrite, MakeWriteTask(&state, &nDone)));
    }

    const bool bDone = WaitUntil(
        [&nDone]()
        {
            return nDone.load() == kTasks;
        },
        10000);
    gate.Close();
    gate.Drain();
    pool.Stop();

    ASSERT_TRUE(bDone);
    ASSERT_TRUE(state.nPeakWriters.load() == 1);  // 写者唯一
    ASSERT_TRUE(state.nViolations.load() == 0);   // 无读写重叠
}

/// @brief 高并发读：32 线程、读上限 16，验证读确实大规模并行且不超上限。
TEST(Gate_HighConcurrencyRead)
{
    const int kThreads = 32;
    const size_t kMaxReaders = 16;
    const int kTasks = 128;

    CThreadPool pool(kThreads);
    ASSERT_TRUE(pool.Start());
    CReadWriteGate gate(&pool, kMaxReaders);

    SGateState state;
    std::atomic<int> nDone(0);
    for (int i = 0; i < kTasks; ++i)
    {
        ASSERT_TRUE(gate.Submit(TaskKind::kRead, MakeReadTask(&state, &nDone)));
    }

    const bool bDone = WaitUntil(
        [&nDone]()
        {
            return nDone.load() == kTasks;
        },
        15000);
    gate.Close();
    gate.Drain();
    pool.Stop();

    ASSERT_TRUE(bDone);
    ASSERT_TRUE(state.nPeakReaders.load() >= 8);                              // 高并发确实发生
    ASSERT_TRUE(state.nPeakReaders.load() <= static_cast<int>(kMaxReaders));  // 不超上限
    ASSERT_TRUE(state.nViolations.load() == 0);
}

/// @brief 顺序（公平 FIFO）：先读后写 —— 写不插队到先前排队的读前面。
///        用占位读占满唯一读槽位，使随后提交的读被迫排队，再提交写；
///        验证执行顺序仍为 读→写（旧的写优先策略会得到 写→读）。
TEST(Gate_FairFifo_ReadThenWrite)
{
    CThreadPool pool(4);
    ASSERT_TRUE(pool.Start());
    CReadWriteGate gate(&pool, 1);  // 唯一读槽位

    std::atomic<int> nPlaceholderActive(0);  // 占位读已活跃
    std::mutex mutex;
    std::vector<int> vecOrder;
    std::atomic<int> nDone(0);

    // 占位读：占住唯一读槽位一段时间（其内提交的读只能排队）。
    ASSERT_TRUE(gate.Submit(TaskKind::kRead,
        [&nPlaceholderActive]()
        {
            nPlaceholderActive.store(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }));
    ASSERT_TRUE(WaitUntil(
        [&nPlaceholderActive]()
        {
            return nPlaceholderActive.load() == 1;
        },
        3000));

    // 先读后写：读因槽位满而排队，写排在读之后 —— 不得越过。
    ASSERT_TRUE(gate.Submit(TaskKind::kRead,
        [&mutex, &vecOrder, &nDone]()
        {
            std::lock_guard<std::mutex> lock(mutex);
            vecOrder.push_back(1);  // 读
            nDone.fetch_add(1);
        }));
    ASSERT_TRUE(gate.Submit(TaskKind::kWrite,
        [&mutex, &vecOrder, &nDone]()
        {
            std::lock_guard<std::mutex> lock(mutex);
            vecOrder.push_back(2);  // 写
            nDone.fetch_add(1);
        }));

    const bool bDone = WaitUntil(
        [&nDone]()
        {
            return nDone.load() == 2;
        },
        3000);
    gate.Close();
    gate.Drain();
    pool.Stop();

    ASSERT_TRUE(bDone);
    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_TRUE(vecOrder.size() == 2);
    ASSERT_TRUE(vecOrder[0] == 1);  // 读在前
    ASSERT_TRUE(vecOrder[1] == 2);  // 写在后（写不插队）
}

/// @brief 顺序（公平 FIFO）：先写后读 —— 读不越过先前提交的写。
TEST(Gate_FairFifo_WriteThenRead)
{
    CThreadPool pool(4);
    ASSERT_TRUE(pool.Start());
    CReadWriteGate gate(&pool);

    std::mutex mutex;
    std::vector<int> vecOrder;
    std::atomic<int> nDone(0);

    ASSERT_TRUE(gate.Submit(TaskKind::kWrite,
        [&mutex, &vecOrder, &nDone]()
        {
            std::lock_guard<std::mutex> lock(mutex);
            vecOrder.push_back(1);  // 写
            nDone.fetch_add(1);
        }));
    ASSERT_TRUE(gate.Submit(TaskKind::kRead,
        [&mutex, &vecOrder, &nDone]()
        {
            std::lock_guard<std::mutex> lock(mutex);
            vecOrder.push_back(2);  // 读
            nDone.fetch_add(1);
        }));

    const bool bDone = WaitUntil(
        [&nDone]()
        {
            return nDone.load() == 2;
        },
        3000);
    gate.Close();
    gate.Drain();
    pool.Stop();

    ASSERT_TRUE(bDone);
    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_TRUE(vecOrder.size() == 2);
    ASSERT_TRUE(vecOrder[0] == 1);  // 写在前
    ASSERT_TRUE(vecOrder[1] == 2);  // 读在后（读不越过写）
}

/// @brief 顺序（公平 FIFO）：写者严格按提交顺序执行（写者 FIFO）。
TEST(Gate_FairFifo_WriterFifo)
{
    CThreadPool pool(4);
    ASSERT_TRUE(pool.Start());
    CReadWriteGate gate(&pool);

    const int kWriters = 8;
    std::mutex mutex;
    std::vector<int> vecOrder;
    std::atomic<int> nDone(0);

    for (int i = 0; i < kWriters; ++i)
    {
        ASSERT_TRUE(gate.Submit(TaskKind::kWrite,
            [&mutex, &vecOrder, &nDone, i]()
            {
                std::lock_guard<std::mutex> lock(mutex);
                vecOrder.push_back(i);  // 记录执行顺序
                nDone.fetch_add(1);
            }));
    }

    const bool bDone = WaitUntil(
        [&nDone]()
        {
            return nDone.load() == kWriters;
        },
        3000);
    gate.Close();
    gate.Drain();
    pool.Stop();

    ASSERT_TRUE(bDone);
    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_TRUE(vecOrder.size() == static_cast<size_t>(kWriters));
    for (int i = 0; i < kWriters; ++i)
    {
        ASSERT_TRUE(vecOrder[static_cast<size_t>(i)] == i);  // 严格按提交顺序
    }
}

/// @brief 同门重入：读任务内再向本门提交写任务（非阻塞 → 无死锁）。
///        写任务排在后面等读者排空，读任务立即返回、不占住线程。
TEST(Gate_ReentrantSubmit)
{
    const int kThreads = 4;
    const int kTasks = 20;

    CThreadPool pool(kThreads);
    ASSERT_TRUE(pool.Start());
    CReadWriteGate gate(&pool, 2);

    SGateState state;
    std::atomic<int> nReadDone(0);
    std::atomic<int> nWriteDone(0);
    for (int i = 0; i < kTasks; ++i)
    {
        ASSERT_TRUE(gate.Submit(TaskKind::kRead,
            [&gate, &state, &nReadDone, &nWriteDone]()
            {
                RunReadWork(&state);
                nReadDone.fetch_add(1);
                // 读任务内再提交同门写任务（写需等读退出）—— 提交是入队、不阻塞。
                gate.Submit(TaskKind::kWrite,
                    [&state, &nWriteDone]()
                    {
                        RunWriteWork(&state);
                        nWriteDone.fetch_add(1);
                    });
            }));
    }

    const bool bDone = WaitUntil(
        [&nReadDone, &nWriteDone, kTasks]()
        {
            return nReadDone.load() == kTasks && nWriteDone.load() == kTasks;
        },
        15000);
    gate.Close();
    gate.Drain();
    pool.Stop();

    ASSERT_TRUE(bDone);
    ASSERT_TRUE(state.nViolations.load() == 0);
}

/// @brief 多门链式调用：A 读 → B 写 → C 写（跨门嵌套）+ B 读。
///        验证跨门调度正确、无死锁、计数精确（不变式按门独立统计）。
TEST(Gate_MultiGateChain)
{
    const int kThreads = 16;
    const int kFlows = 200;

    CThreadPool pool(kThreads);
    ASSERT_TRUE(pool.Start());
    CReadWriteGate gateA(&pool, 4);  // 模块 A：读
    CReadWriteGate gateB(&pool, 3);  // 模块 B：读写混合
    CReadWriteGate gateC(&pool);     // 模块 C：写

    SGateState stateA;
    SGateState stateB;
    SGateState stateC;
    std::atomic<int> nReadDone(0);
    std::atomic<int> nWriteDone(0);

    for (int i = 0; i < kFlows; ++i)
    {
        // 模块 A：读任务（其内链式调用 B 的写 → C 的写）
        ASSERT_TRUE(gateA.Submit(TaskKind::kRead,
            [&gateB, &gateC, &stateA, &stateB, &stateC, &nReadDone, &nWriteDone]()
            {
                RunReadWork(&stateA);
                nReadDone.fetch_add(1);
                gateB.Submit(TaskKind::kWrite,
                    [&gateC, &stateB, &stateC, &nWriteDone]()
                    {
                        RunWriteWork(&stateB);
                        nWriteDone.fetch_add(1);
                        gateC.Submit(TaskKind::kWrite,
                            [&stateC, &nWriteDone]()
                            {
                                RunWriteWork(&stateC);
                                nWriteDone.fetch_add(1);
                            });
                    });
            }));
        // 模块 B：读任务（与 B 的写竞争：写独占、读排队）
        ASSERT_TRUE(gateB.Submit(TaskKind::kRead, MakeReadTask(&stateB, &nReadDone)));
    }

    // 每流程：A 读 + B 读 = 2 读；B 写 + C 写 = 2 写
    const bool bDone = WaitUntil(
        [&nReadDone, &nWriteDone, kFlows]()
        {
            return nReadDone.load() == kFlows * 2 && nWriteDone.load() == kFlows * 2;
        },
        30000);
    gateA.Close();
    gateB.Close();
    gateC.Close();
    gateA.Drain();
    gateB.Drain();
    gateC.Drain();
    pool.Stop();

    ASSERT_TRUE(bDone);
    ASSERT_TRUE(stateA.nPeakReaders.load() >= 2);  // A 的读确实并发
    ASSERT_TRUE(stateB.nPeakWriters.load() == 1);  // B 的写唯一
    ASSERT_TRUE(stateA.nViolations.load() == 0);
    ASSERT_TRUE(stateB.nViolations.load() == 0);
    ASSERT_TRUE(stateC.nViolations.load() == 0);
}

/// @brief 多生产者并发投递：8 个线程同时提交，验证并发提交路径无竞争、
///        多生产者下仍保持读写互斥与精确计数。
TEST(Gate_MultiProducerSubmit)
{
    const int kThreads = 32;
    const int kProducers = 8;
    const int kTasksPerProducer = 500;
    const int kGates = 4;
    const int kTotal = kProducers * kTasksPerProducer;

    CThreadPool pool(kThreads);
    ASSERT_TRUE(pool.Start());

    std::vector<std::unique_ptr<CReadWriteGate> > vecGates;
    for (int m = 0; m < kGates; ++m)
    {
        vecGates.push_back(std::unique_ptr<CReadWriteGate>(new CReadWriteGate(&pool, 8)));
    }
    SGateState state[kGates];
    std::atomic<int> nReadDone(0);
    std::atomic<int> nWriteDone(0);
    std::atomic<int> nSubmitFail(0);

    std::vector<std::thread> vecProducers;
    for (int p = 0; p < kProducers; ++p)
    {
        vecProducers.push_back(std::thread(
            [&, p]()
            {
                for (int i = 0; i < kTasksPerProducer; ++i)
                {
                    const int nReadGate = (p + i) % kGates;
                    const int nWriteGate = (p + i + 1) % kGates;
                    if (!vecGates[nReadGate]->Submit(TaskKind::kRead, MakeReadTask(&state[nReadGate], &nReadDone)))
                    {
                        nSubmitFail.fetch_add(1);
                    }
                    if (!vecGates[nWriteGate]->Submit(TaskKind::kWrite, MakeWriteTask(&state[nWriteGate], &nWriteDone)))
                    {
                        nSubmitFail.fetch_add(1);
                    }
                }
            }));
    }
    for (std::vector<std::thread>::iterator it = vecProducers.begin(); it != vecProducers.end(); ++it)
    {
        it->join();
    }

    const bool bDone = WaitUntil(
        [&nReadDone, &nWriteDone, kTotal]()
        {
            return nReadDone.load() == kTotal && nWriteDone.load() == kTotal;
        },
        30000);
    for (int m = 0; m < kGates; ++m)
    {
        vecGates[m]->Close();
        vecGates[m]->Drain();
    }
    pool.Stop();

    ASSERT_TRUE(bDone);
    ASSERT_TRUE(nSubmitFail.load() == 0);  // 全部投递成功
    for (int m = 0; m < kGates; ++m)
    {
        ASSERT_TRUE(state[m].nPeakWriters.load() == 1);  // 各门写唯一
        ASSERT_TRUE(state[m].nViolations.load() == 0);   // 无违例
    }
}

/// @brief 多门高并发压力：64 线程、16 门、8000 任务确定性散布读写。
///        逐门验证：读不超上限、写唯一、无违例；全局计数精确。
TEST(Gate_ManyGatesStress)
{
    const int kThreads = 64;
    const int kGates = 16;
    const int kTasks = 8000;

    CThreadPool pool(kThreads);
    ASSERT_TRUE(pool.Start());

    std::vector<std::unique_ptr<CReadWriteGate> > vecGates;
    std::vector<int> vecReadCaps;
    for (int m = 0; m < kGates; ++m)
    {
        const int nCap = 4 + (m % 4);
        vecGates.push_back(std::unique_ptr<CReadWriteGate>(new CReadWriteGate(&pool, static_cast<size_t>(nCap))));
        vecReadCaps.push_back(nCap);
    }
    SGateState state[kGates];
    std::atomic<int> nReadDone(0);
    std::atomic<int> nWriteDone(0);

    for (int i = 0; i < kTasks; ++i)
    {
        // 确定性散布到各门（可复现）：16 与 5/7 互质 → 每个门都会被覆盖。
        const int nReadGate = (i * 5 + 1) % kGates;
        const int nWriteGate = (i * 7 + 3) % kGates;
        ASSERT_TRUE(vecGates[nReadGate]->Submit(TaskKind::kRead, MakeReadTask(&state[nReadGate], &nReadDone)));
        ASSERT_TRUE(vecGates[nWriteGate]->Submit(TaskKind::kWrite, MakeWriteTask(&state[nWriteGate], &nWriteDone)));
    }

    const bool bDone = WaitUntil(
        [&nReadDone, &nWriteDone, kTasks]()
        {
            return nReadDone.load() == kTasks && nWriteDone.load() == kTasks;
        },
        60000);
    for (int m = 0; m < kGates; ++m)
    {
        vecGates[m]->Close();
        vecGates[m]->Drain();
    }
    pool.Stop();

    ASSERT_TRUE(bDone);
    for (int m = 0; m < kGates; ++m)
    {
        ASSERT_TRUE(state[m].nPeakReaders.load() <= vecReadCaps[m]);  // 读不超上限
        ASSERT_TRUE(state[m].nPeakWriters.load() == 1);               // 写唯一
        ASSERT_TRUE(state[m].nViolations.load() == 0);                // 无违例
    }
}

/// @brief 排空：Drain 等全部任务结束后返回，门空闲、计数精确（无需 Close 也可排空）。
TEST(Gate_DrainIdle)
{
    CThreadPool pool(4);
    ASSERT_TRUE(pool.Start());
    CReadWriteGate gate(&pool);

    const int kTasks = 20;
    SGateState state;
    std::atomic<int> nDone(0);
    for (int i = 0; i < kTasks; ++i)
    {
        ASSERT_TRUE(gate.Submit(TaskKind::kWrite, MakeWriteTask(&state, &nDone)));
    }

    gate.Drain();  // 本用例不再投递：等的就是这一批

    ASSERT_TRUE(nDone.load() == kTasks);
    ASSERT_TRUE(gate.PendingCount() == 0);
    ASSERT_TRUE(gate.IsIdle());
    ASSERT_TRUE(gate.ActiveReaders() == 0);
    ASSERT_TRUE(!gate.HasActiveWriter());

    gate.Close();
    pool.Stop();
}

/// @brief 拒绝路径：线程池未启动 → 不接受；Close 之后 → 不接受；
///        已接受的任务照旧跑完（Close 不取消已入队的任务）。
TEST(Gate_SubmitRejectedWhenPoolDown)
{
    CThreadPool pool(2);  // 故意不启动
    CReadWriteGate gate(&pool);

    ASSERT_TRUE(!gate.Submit(TaskKind::kRead,
        []()
        {
        }));  // 线程池未启动 → 不接受

    ASSERT_TRUE(pool.Start());
    SGateState state;
    std::atomic<int> nDone(0);
    ASSERT_TRUE(gate.Submit(TaskKind::kWrite, MakeWriteTask(&state, &nDone)));

    gate.Close();  // 关闭后拒新投递
    ASSERT_TRUE(gate.IsClosed());
    ASSERT_TRUE(!gate.Submit(TaskKind::kRead,
        []()
        {
        }));

    gate.Drain();  // 已接受的任务照旧跑完
    pool.Stop();

    ASSERT_TRUE(nDone.load() == 1);
    ASSERT_TRUE(gate.IsIdle());
}

/// @brief 任务抛异常：槽位仍归还（后续写任务能拿到独占），并报告诊断。
///        若归位被异常绕过，写任务会永远等不到「读者排空」。
TEST(Gate_SlotReturnedOnTaskThrow)
{
    CThreadPool pool(2);
    ASSERT_TRUE(pool.Start());
    CReadWriteGate gate(&pool);

    std::atomic<int> nThrowReports(0);
    common::async::SetDiagnosticHandler(
        [&nThrowReports](const char* /*strWhat*/)
        {
            nThrowReports.fetch_add(1);
        });

    std::atomic<int> nWriteDone(0);
    ASSERT_TRUE(gate.Submit(TaskKind::kRead,
        []()
        {
            throw std::runtime_error("读任务异常");
        }));
    ASSERT_TRUE(gate.Submit(TaskKind::kWrite,
        [&nWriteDone]()
        {
            nWriteDone.fetch_add(1);
        }));

    // 写任务能跑起来 = 读任务的槽位确实归还了。
    const bool bWriteRan = WaitUntil(
        [&nWriteDone]()
        {
            return nWriteDone.load() == 1;
        },
        3000);
    gate.Close();
    gate.Drain();
    pool.Stop();
    common::async::SetDiagnosticHandler(nullptr);  // 还原默认（debug 打 stderr / release 安静）

    ASSERT_TRUE(bWriteRan);
    ASSERT_TRUE(nThrowReports.load() >= 1);  // 异常被门兜住并报告
    ASSERT_TRUE(gate.IsIdle());
}
