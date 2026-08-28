/// @file test_concurrency.cpp
/// 协程/任务框架严格并发安全测试。
///
/// 目标：真多线程（std::thread + 自旋屏障同步起跑）压测并发路径，
/// 用「原子精确断言 / 无重复无丢失」验证线程安全：
///  - 并发 CoStart/Get 同一执行器、多执行器并行
///  - 共享原子计数器精确累加（回调无重复/丢失）
///  - 并行 await 并发回调访问共享结构（互斥保护）
///  - 多生产者/消费者互斥队列严格交换
///  - 生命周期极端：并发释放协程 + 并发 Start/Stop
///  - 多线程 Get 同一任务（框架须 notify_all 唤醒全部等待者）
///
/// 注意：子线程内不使用 ASSERT 宏（其抛异常无法跨线程捕获），
/// 统一用 nFail 原子计数，主线程 join 后断言。

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "TestFramework.h"

#include "Async/AsyncExecutor.h"
#include "Async/Coroutine.h"

namespace {

// ------------------------------------------------------------------
// 自旋屏障：让多线程同时起跑，增强竞争强度（真并发）。
// ------------------------------------------------------------------

class CSpinBarrier
{
public:
    explicit CSpinBarrier(int nCount) : m_nCount(nCount), m_nArrived(0), m_nGeneration(0) {}

    void Wait()
    {
        int nGen = m_nGeneration.load(std::memory_order_acquire);
        if (m_nArrived.fetch_add(1, std::memory_order_acq_rel) + 1 == m_nCount)
        {
            m_nArrived.store(0, std::memory_order_relaxed);
            m_nGeneration.fetch_add(1, std::memory_order_release);
        }
        else
        {
            while (m_nGeneration.load(std::memory_order_acquire) == nGen)
            {
                std::this_thread::yield();
            }
        }
    }

private:
    int m_nCount;
    std::atomic<int> m_nArrived;
    std::atomic<int> m_nGeneration;
};

// ------------------------------------------------------------------
// 并发测试用协程类
// ------------------------------------------------------------------

/// 单 await 返回固定值 7（通用并发实例）。
class CConcSeqCoro : public common::async::CCoroutine<int>
{
public:
    CConcSeqCoro() : m_nV(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_INTO(m_nV, []() { return 7; });
        CO_RETURN(m_nV);
        CO_END();
    }

private:
    int m_nV;
};

/// 返回期望值（实例状态隔离验证用：各实例独立，无串扰）。
class CConcValCoro : public common::async::CCoroutine<int>
{
public:
    explicit CConcValCoro(int nExpected) : m_nExpected(nExpected), m_nV(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_INTO(m_nV, [this]() { return m_nExpected; });
        CO_RETURN(m_nV);
        CO_END();
    }

private:
    int m_nExpected;
    int m_nV;
};

/// await 后对共享原子计数器 fetch_add(1)（回调无重复/丢失检测）。
class CConcIncCoro : public common::async::CCoroutine<void>
{
public:
    explicit CConcIncCoro(std::atomic<int>* pCounter) : m_pCounter(pCounter) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT([this]() { m_pCounter->fetch_add(1, std::memory_order_relaxed); });
        CO_RETURN_VOID();
        CO_END();
    }

private:
    std::atomic<int>* m_pCounter;
};

/// CO_AWAIT_ALL 4 个任务，各加 m_nGroup 到共享原子（并行回调并发累加）。
class CConcAllIncCoro : public common::async::CCoroutine<void>
{
public:
    CConcAllIncCoro(std::atomic<int>* pCounter, int nGroup)
        : m_pCounter(pCounter), m_nGroup(nGroup) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_ALL([this]() { m_pCounter->fetch_add(m_nGroup, std::memory_order_relaxed); },
                     [this]() { m_pCounter->fetch_add(m_nGroup, std::memory_order_relaxed); },
                     [this]() { m_pCounter->fetch_add(m_nGroup, std::memory_order_relaxed); },
                     [this]() { m_pCounter->fetch_add(m_nGroup, std::memory_order_relaxed); });
        CO_RETURN_VOID();
        CO_END();
    }

private:
    std::atomic<int>* m_pCounter;
    int m_nGroup;
};

/// CO_AWAIT_ALL 并行回调写互斥集合 + 原子和（共享结构并发访问检测）。
class CConcAllCollectCoro : public common::async::CCoroutine<int>
{
public:
    CConcAllCollectCoro(std::mutex* pMutex, std::vector<int>* pVec, std::atomic<int>* pSum)
        : m_pMutex(pMutex), m_pVec(pVec), m_pSum(pSum), m_nTotal(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_ALL([this]() { Add(1); },
                     [this]() { Add(2); },
                     [this]() { Add(3); },
                     [this]() { Add(4); });
        CO_RETURN(m_nTotal); // 1+2+3+4 = 10
        CO_END();
    }

private:
    void Add(int n)
    {
        std::lock_guard<std::mutex> lock(*m_pMutex); // 共享结构互斥保护。
        m_pVec->push_back(n);
        m_nTotal += n;
        m_pSum->fetch_add(n, std::memory_order_relaxed);
    }

    std::mutex* m_pMutex;
    std::vector<int>* m_pVec;
    std::atomic<int>* m_pSum;
    int m_nTotal;
};

// ------------------------------------------------------------------
// 多生产者/消费者互斥队列
// ------------------------------------------------------------------

/// 互斥保护的有界 int 队列（严格交换验证：无丢失、无重复）。
class CSharedQueue
{
public:
    void Push(int n)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(n);
    }

    bool TryPop(int& nOut)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty())
        {
            return false;
        }
        nOut = m_queue.front();
        m_queue.pop_front();
        return true;
    }

    size_t Size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

private:
    mutable std::mutex m_mutex;
    std::deque<int> m_queue;
};

/// 生产者：await 任务内并发 push nPer 条（id 唯一：base+i）。
class CConcProducerCoro : public common::async::CCoroutine<void>
{
public:
    CConcProducerCoro(const std::shared_ptr<CSharedQueue>& spQueue, int nBase, int nPer)
        : m_spQueue(spQueue), m_nBase(nBase), m_nPer(nPer) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT([this]()
        {
            for (int i = 0; i < m_nPer; ++i)
            {
                m_spQueue->Push(m_nBase + i);
            }
        });
        CO_RETURN_VOID();
        CO_END();
    }

private:
    std::shared_ptr<CSharedQueue> m_spQueue;
    int m_nBase;
    int m_nPer;
};

/// 消费者：await 任务内并发 TryPop 拉空队列，结果写入共享集合。
class CConcConsumerCoro : public common::async::CCoroutine<void>
{
public:
    CConcConsumerCoro(const std::shared_ptr<CSharedQueue>& spQueue,
                      std::mutex* pMutex, std::vector<int>* pConsumed)
        : m_spQueue(spQueue), m_pMutex(pMutex), m_pConsumed(pConsumed) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT([this]()
        {
            int n = 0;
            while (m_spQueue->TryPop(n))
            {
                std::lock_guard<std::mutex> lock(*m_pMutex);
                m_pConsumed->push_back(n);
            }
        });
        CO_RETURN_VOID();
        CO_END();
    }

private:
    std::shared_ptr<CSharedQueue> m_spQueue;
    std::mutex* m_pMutex;
    std::vector<int>* m_pConsumed;
};

} // namespace

// ------------------------------------------------------------------
// 严格并发安全测试
// ------------------------------------------------------------------

/// @brief 8 线程并发 CoStart/Get 同一执行器：全部结果正确。
TEST(Concurrency_MultiThreadCoStart)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    const int nThreads = 8;
    const int nPerThread = 2000;
    std::atomic<int> nFail(0);
    CSpinBarrier barrier(nThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < nThreads; ++t)
    {
        threads.push_back(std::thread([&]()
        {
            barrier.Wait();
            for (int i = 0; i < nPerThread; ++i)
            {
                std::shared_ptr<CConcSeqCoro> pCoro = exec.CoStart<CConcSeqCoro>();
                common::async::CTaskResult<int> r = pCoro->Get();
                if (!r.HasValue() || r.Value() != 7)
                {
                    nFail.fetch_add(1);
                }
            }
        }));
    }
    for (size_t i = 0; i < threads.size(); ++i)
    {
        threads[i].join();
    }
    ASSERT_EQ(nFail.load(), 0);
    exec.Stop();
}

/// @brief 20000 协程并发对共享原子计数器累加：精确（回调无重复/丢失）。
TEST(Concurrency_SharedCounterExact)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    std::atomic<int> nCounter(0);
    const int nTotal = 20000;
    std::vector<std::shared_ptr<CConcIncCoro> > vCoros;
    vCoros.reserve(nTotal);
    for (int i = 0; i < nTotal; ++i)
    {
        vCoros.push_back(exec.CoStart<CConcIncCoro>(&nCounter));
    }
    for (int i = 0; i < nTotal; ++i)
    {
        ASSERT_TRUE(vCoros[i]->Get().HasValue());
    }
    ASSERT_EQ(nCounter.load(), nTotal); // 精确：无丢失更新。
    exec.Stop();
}

/// @brief 8 线程并发实例隔离：各实例返回各自期望值，无串扰/竞争。
TEST(Concurrency_ResultIsolation)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    const int nThreads = 8;
    const int nPerThread = 1500;
    std::atomic<int> nFail(0);
    CSpinBarrier barrier(nThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < nThreads; ++t)
    {
        threads.push_back(std::thread([&, t]()
        {
            barrier.Wait();
            for (int i = 0; i < nPerThread; ++i)
            {
                int nExpected = t * 1000000 + i;
                std::shared_ptr<CConcValCoro> pCoro =
                    exec.CoStart<CConcValCoro>(nExpected);
                common::async::CTaskResult<int> r = pCoro->Get();
                if (!r.HasValue() || r.Value() != nExpected)
                {
                    nFail.fetch_add(1);
                }
            }
        }));
    }
    for (size_t i = 0; i < threads.size(); ++i)
    {
        threads[i].join();
    }
    ASSERT_EQ(nFail.load(), 0);
    exec.Stop();
}

/// @brief 8 个执行器并行运行，各自跑协程互不干扰。
TEST(Concurrency_MultiExecutorParallel)
{
    const int nExecs = 8;
    const int nPerExec = 1000;
    std::atomic<int> nFail(0);
    CSpinBarrier barrier(nExecs);
    std::vector<std::thread> threads;
    for (int e = 0; e < nExecs; ++e)
    {
        threads.push_back(std::thread([&]()
        {
            barrier.Wait();
            common::async::CAsyncExecutor exec(2);
            if (!exec.Start())
            {
                nFail.fetch_add(1);
                return;
            }
            for (int i = 0; i < nPerExec; ++i)
            {
                std::shared_ptr<CConcSeqCoro> pCoro = exec.CoStart<CConcSeqCoro>();
                common::async::CTaskResult<int> r = pCoro->Get();
                if (!r.HasValue() || r.Value() != 7)
                {
                    nFail.fetch_add(1);
                }
            }
            exec.Stop();
        }));
    }
    for (size_t i = 0; i < threads.size(); ++i)
    {
        threads[i].join();
    }
    ASSERT_EQ(nFail.load(), 0);
}

/// @brief 并行 await 并发回调写共享结构（互斥集合 + 原子和）：精确一致。
TEST(Concurrency_AwaitAllSharedState)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    const int nCoros = 2000;
    std::mutex mutex;
    std::vector<int> vCollected;
    std::atomic<int> nSum(0);
    std::vector<std::shared_ptr<CConcAllCollectCoro> > vCoros;
    vCoros.reserve(nCoros);
    for (int i = 0; i < nCoros; ++i)
    {
        vCoros.push_back(exec.CoStart<CConcAllCollectCoro>(&mutex, &vCollected, &nSum));
    }
    for (int i = 0; i < nCoros; ++i)
    {
        common::async::CTaskResult<int> r = vCoros[i]->Get();
        ASSERT_TRUE(r.HasValue());
        ASSERT_EQ(r.Value(), 10); // 1+2+3+4
    }
    ASSERT_EQ(vCollected.size(), static_cast<size_t>(nCoros * 4));
    ASSERT_EQ(nSum.load(), nCoros * 10);
    exec.Stop();
}

/// @brief 并发协程各执行 CO_AWAIT_ALL（4 路）：共享原子精确累加（无丢失）。
TEST(Concurrency_ParallelAwaitAllStress)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    const int nCoros = 1000;
    std::atomic<int> nCounter(0);
    std::vector<std::shared_ptr<CConcAllIncCoro> > vCoros;
    vCoros.reserve(nCoros);
    for (int i = 0; i < nCoros; ++i)
    {
        vCoros.push_back(exec.CoStart<CConcAllIncCoro>(&nCounter, i + 1));
    }
    for (int i = 0; i < nCoros; ++i)
    {
        ASSERT_TRUE(vCoros[i]->Get().HasValue());
    }
    // 每个协程 4 个任务各加 (i+1)：总 = 4 * sum(1..nCoros)。
    long nExpected = 4L * (static_cast<long>(nCoros) * (nCoros + 1)) / 2;
    ASSERT_EQ(nCounter.load(), static_cast<int>(nExpected));
    exec.Stop();
}

/// @brief 多生产者/消费者互斥队列严格交换：无丢失、无重复（id 恰好出现一次）。
TEST(Concurrency_ProducerConsumerQueue)
{
    const int nProducers = 8;
    const int nConsumers = 8;
    const int nPer = 500;
    const int nTotal = nProducers * nPer;

    std::shared_ptr<CSharedQueue> spQueue = std::make_shared<CSharedQueue>();
    std::mutex mutex;
    std::vector<int> vConsumed;

    {
        common::async::CAsyncExecutor exec(8);
        ASSERT_TRUE(exec.Start());

        std::vector<std::shared_ptr<CConcProducerCoro> > vP;
        vP.reserve(nProducers);
        for (int p = 0; p < nProducers; ++p)
        {
            vP.push_back(exec.CoStart<CConcProducerCoro>(spQueue, p * nPer, nPer));
        }
        for (int p = 0; p < nProducers; ++p)
        {
            ASSERT_TRUE(vP[p]->Get().HasValue());
        }
        ASSERT_EQ(spQueue->Size(), static_cast<size_t>(nTotal));

        std::vector<std::shared_ptr<CConcConsumerCoro> > vC;
        vC.reserve(nConsumers);
        for (int c = 0; c < nConsumers; ++c)
        {
            vC.push_back(exec.CoStart<CConcConsumerCoro>(spQueue, &mutex, &vConsumed));
        }
        for (int c = 0; c < nConsumers; ++c)
        {
            ASSERT_TRUE(vC[c]->Get().HasValue());
        }
        exec.Stop();
    }

    // 严格：每个 id 恰好消费一次（无丢失、无重复）。
    ASSERT_EQ(vConsumed.size(), static_cast<size_t>(nTotal));
    std::vector<int> vCount(nTotal, 0);
    for (size_t i = 0; i < vConsumed.size(); ++i)
    {
        vCount[vConsumed[i]]++;
    }
    for (int i = 0; i < nTotal; ++i)
    {
        ASSERT_EQ(vCount[i], 1);
    }
}

/// @brief 8 线程并发创建并立即释放协程（不 Get）：析构与 Resume 并发安全。
TEST(Concurrency_LifetimeConcurrentRelease)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    const int nThreads = 8;
    const int nPerThread = 1000;
    std::atomic<int> nFail(0);
    CSpinBarrier barrier(nThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < nThreads; ++t)
    {
        threads.push_back(std::thread([&]()
        {
            barrier.Wait();
            for (int i = 0; i < nPerThread; ++i)
            {
                std::shared_ptr<CConcSeqCoro> pCoro = exec.CoStart<CConcSeqCoro>();
                // 作用域结束即释放（不 Get）：验证析构与 Resume 并发不悬垂。
            }
        }));
    }
    for (size_t i = 0; i < threads.size(); ++i)
    {
        threads[i].join();
    }
    exec.Stop(); // 等待在飞 Resume 完成。
    ASSERT_EQ(nFail.load(), 0);
}

/// @brief 4 线程并发反复 Start/Stop 执行器 + 协程在飞：安全终止不崩溃。
TEST(Concurrency_ConcurrentStopStart)
{
    const int nThreads = 4;
    std::atomic<int> nFail(0);
    std::vector<std::thread> threads;
    for (int t = 0; t < nThreads; ++t)
    {
        threads.push_back(std::thread([&]()
        {
            for (int r = 0; r < 15; ++r)
            {
                common::async::CAsyncExecutor exec(2);
                if (!exec.Start())
                {
                    nFail.fetch_add(1);
                    continue;
                }
                std::vector<std::shared_ptr<CConcSeqCoro> > v;
                v.reserve(200);
                for (int i = 0; i < 200; ++i)
                {
                    v.push_back(exec.CoStart<CConcSeqCoro>());
                }
                exec.Stop();
                // 协程以完成（7）或 kStopped（Stop 时挂起）终止，均安全。
                for (size_t i = 0; i < v.size(); ++i)
                {
                    common::async::CTaskResult<int> r = v[i]->Get();
                    if (r.HasValue() && r.Value() != 7)
                    {
                        nFail.fetch_add(1);
                    }
                }
            }
        }));
    }
    for (size_t i = 0; i < threads.size(); ++i)
    {
        threads[i].join();
    }
    ASSERT_EQ(nFail.load(), 0);
}

/// @brief 8 线程同时 Get 同一任务结果：notify_all 唤醒全部等待者。
TEST(Concurrency_ConcurrentGetSameTask)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<common::async::CTask<int> > spTask =
        std::make_shared<common::async::CTask<int> >(exec.Submit([]() { return 42; }));

    const int nThreads = 8;
    std::atomic<int> nFail(0);
    CSpinBarrier barrier(nThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < nThreads; ++t)
    {
        threads.push_back(std::thread([&]()
        {
            barrier.Wait();
            common::async::CTaskResult<int> r = spTask->Get();
            if (!r.HasValue() || r.Value() != 42)
            {
                nFail.fetch_add(1);
            }
        }));
    }
    for (size_t i = 0; i < threads.size(); ++i)
    {
        threads[i].join();
    }
    ASSERT_EQ(nFail.load(), 0);
    exec.Stop();
}
