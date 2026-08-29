/// @file test_stress.cpp
/// 协程/任务框架压力测试（极限负载下的稳定性与正确性）。
///
/// 目标：在远超常规的量级下验证框架不崩溃、不挂起、结果保持正确：
///  - 海量协程批量（内存 + 调度压力）
///  - 海量任务堆积（backlog）
///  - 海量并行组（CO_AWAIT_ALL）
///  - 深层嵌套链（4 层 AsTask）
///  - 执行器多轮重建（资源回收稳定）
///  - 多线程多执行器混合负载
///
/// 断言：结果正确性严格断言；量级按本机能力取适中值，控制在秒级。

#include <atomic>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "TestFramework.h"

#include "Async/AsyncExecutor.h"
#include "Async/Coroutine.h"

namespace {

// ------------------------------------------------------------------
// 压力测试用协程类
// ------------------------------------------------------------------

/// 单 await 返回期望值（批量压力用）。
class CStrSeqCoro : public common::async::CCoroutine<int>
{
public:
    explicit CStrSeqCoro(int nExpected) : m_nExpected(nExpected), m_nV(0) {}

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

/// 8 路并行 await（并行组压力用）。
class CStrAllCoro : public common::async::CCoroutine<int>
{
public:
    CStrAllCoro()
        : m_n1(0), m_n2(0), m_n3(0), m_n4(0),
          m_n5(0), m_n6(0), m_n7(0), m_n8(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_ALL_INTO(m_n1, []() { return 1; },
                          m_n2, []() { return 2; },
                          m_n3, []() { return 3; },
                          m_n4, []() { return 4; },
                          m_n5, []() { return 5; },
                          m_n6, []() { return 6; },
                          m_n7, []() { return 7; },
                          m_n8, []() { return 8; });
        CO_RETURN(m_n1 + m_n2 + m_n3 + m_n4 + m_n5 + m_n6 + m_n7 + m_n8); // 36
        CO_END();
    }

private:
    int m_n1, m_n2, m_n3, m_n4, m_n5, m_n6, m_n7, m_n8;
};

// ---- 4 层嵌套链（深层嵌套压力）----

/// 第 1 层：产出 2。
class CStrN1Coro : public common::async::CCoroutine<int>
{
public:
    CStrN1Coro() : m_nV(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_INTO(m_nV, []() { return 2; });
        CO_RETURN(m_nV);
        CO_END();
    }

private:
    int m_nV;
};

/// 第 2 层：await 第 1 层，乘 3（→ 6）。
class CStrN2Coro : public common::async::CCoroutine<int>
{
public:
    explicit CStrN2Coro(const std::shared_ptr<CStrN1Coro>& sp)
        : m_sp(sp), m_nR(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_INTO(m_nR, m_sp->AsTask());
        CO_RETURN(m_nR * 3);
        CO_END();
    }

private:
    std::shared_ptr<CStrN1Coro> m_sp;
    int m_nR;
};

/// 第 3 层：await 第 2 层，乘 4（→ 24）。
class CStrN3Coro : public common::async::CCoroutine<int>
{
public:
    explicit CStrN3Coro(const std::shared_ptr<CStrN2Coro>& sp)
        : m_sp(sp), m_nR(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_INTO(m_nR, m_sp->AsTask());
        CO_RETURN(m_nR * 4);
        CO_END();
    }

private:
    std::shared_ptr<CStrN2Coro> m_sp;
    int m_nR;
};

/// 第 4 层：await 第 3 层，乘 5（→ 120）。
class CStrN4Coro : public common::async::CCoroutine<int>
{
public:
    explicit CStrN4Coro(const std::shared_ptr<CStrN3Coro>& sp)
        : m_sp(sp), m_nR(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_INTO(m_nR, m_sp->AsTask());
        CO_RETURN(m_nR * 5);
        CO_END();
    }

private:
    std::shared_ptr<CStrN3Coro> m_sp;
    int m_nR;
};

// ------------------------------------------------------------------
// 辅助：进程内存（RSS）读取，Linux /proc/self/status
// ------------------------------------------------------------------

/// @brief 读取当前进程 RSS（KB）；失败返回 -1。
long GetRssKB()
{
    std::ifstream f("/proc/self/status");
    std::string strLine;
    while (std::getline(f, strLine))
    {
        if (strLine.compare(0, 6, "VmRSS:") == 0)
        {
            long nKB = -1;
            std::sscanf(strLine.c_str() + 6, "%ld", &nKB);
            return nKB;
        }
    }
    return -1;
}

} // namespace

// ------------------------------------------------------------------
// 压力测试
// ------------------------------------------------------------------

/// @brief 海量协程批量（50000）：内存 + 调度压力，全部结果正确。
/// 附运行前后 RSS 报告（信息性，不断言）。
TEST(Stress_HugeBatchCoroutines)
{
    const long nRssBefore = GetRssKB();

    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    const int nTotal = 50000;
    std::vector<std::shared_ptr<CStrSeqCoro> > vCoros;
    vCoros.reserve(nTotal);
    for (int i = 0; i < nTotal; ++i)
    {
        vCoros.push_back(exec.CoStart<CStrSeqCoro>(7));
    }
    for (int i = 0; i < nTotal; ++i)
    {
        common::async::CTaskResult<int> r = vCoros[i]->Get();
        ASSERT_TRUE(r.HasValue());
        ASSERT_EQ(r.Value(), 7);
    }
    exec.Stop();
    vCoros.clear();

    const long nRssAfter = GetRssKB();
    std::printf("[STRESS] %-30s before=%ldKB after=%ldKB\n",
                "RSS (batch 50000)", nRssBefore, nRssAfter);
}

/// @brief 海量任务堆积（100000 一次性 Submit）：backlog 压力，逐个精确校验。
TEST(Stress_TaskBacklog)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    const long nTotal = 100000;
    std::vector<common::async::CTask<long> > vTasks;
    vTasks.reserve(static_cast<size_t>(nTotal));
    for (long i = 0; i < nTotal; ++i)
    {
        vTasks.push_back(exec.Submit([i]() { return i % 7; }));
    }
    for (long i = 0; i < nTotal; ++i)
    {
        common::async::CTaskResult<long> r = vTasks[i].Get();
        ASSERT_TRUE(r.HasValue());
        ASSERT_EQ(r.Value(), i % 7);
    }
    exec.Stop();
}

/// @brief 海量并行组（5000 组 × 8 路 = 40000 任务）：并行组压力，精确求和。
TEST(Stress_ParallelGroups)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    const int nGroups = 5000;
    std::vector<std::shared_ptr<CStrAllCoro> > vCoros;
    vCoros.reserve(nGroups);
    for (int i = 0; i < nGroups; ++i)
    {
        vCoros.push_back(exec.CoStart<CStrAllCoro>());
    }
    long nSum = 0;
    for (int i = 0; i < nGroups; ++i)
    {
        common::async::CTaskResult<int> r = vCoros[i]->Get();
        ASSERT_TRUE(r.HasValue());
        ASSERT_EQ(r.Value(), 36);
        nSum += r.Value();
    }
    ASSERT_EQ(nSum, 36L * nGroups);
    exec.Stop();
}

/// @brief 深层嵌套链（2000 条 × 4 层 = 8000 协程）：AsTask 逐级 await 压力。
TEST(Stress_DeepNesting)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    const int nChains = 2000;
    std::vector<std::shared_ptr<CStrN1Coro> > v1;
    std::vector<std::shared_ptr<CStrN2Coro> > v2;
    std::vector<std::shared_ptr<CStrN3Coro> > v3;
    std::vector<std::shared_ptr<CStrN4Coro> > v4;
    v1.reserve(nChains);
    v2.reserve(nChains);
    v3.reserve(nChains);
    v4.reserve(nChains);
    for (int i = 0; i < nChains; ++i)
    {
        v1.push_back(exec.CoStart<CStrN1Coro>());
        v2.push_back(exec.CoStart<CStrN2Coro>(v1.back()));
        v3.push_back(exec.CoStart<CStrN3Coro>(v2.back()));
        v4.push_back(exec.CoStart<CStrN4Coro>(v3.back()));
    }
    for (int i = 0; i < nChains; ++i)
    {
        common::async::CTaskResult<int> r = v4[i]->Get();
        ASSERT_TRUE(r.HasValue());
        ASSERT_EQ(r.Value(), 120); // 2*3*4*5
    }
    exec.Stop();
}

/// @brief 执行器多轮重建（40 轮 × 1000 协程）：资源回收稳定，不累积退化。
TEST(Stress_ManyRoundsReuse)
{
    const int nRounds = 40;
    const int nPerRound = 1000;
    std::atomic<int> nFail(0);
    for (int round = 0; round < nRounds; ++round)
    {
        common::async::CAsyncExecutor exec(2);
        if (!exec.Start())
        {
            nFail.fetch_add(1);
            continue;
        }
        std::vector<std::shared_ptr<CStrSeqCoro> > v;
        v.reserve(nPerRound);
        for (int i = 0; i < nPerRound; ++i)
        {
            v.push_back(exec.CoStart<CStrSeqCoro>(7));
        }
        for (int i = 0; i < nPerRound; ++i)
        {
            common::async::CTaskResult<int> r = v[i]->Get();
            if (!r.HasValue() || r.Value() != 7)
            {
                nFail.fetch_add(1);
            }
        }
        exec.Stop();
    }
    ASSERT_EQ(nFail.load(), 0);
}

/// @brief 混合负载：4 线程 × 4 执行器，每轮混跑顺序/并行/任务链，多轮精确校验。
TEST(Stress_HybridMixedLoad)
{
    const int nThreads = 4;
    const int nRounds = 50;
    std::atomic<int> nFail(0);
    std::vector<std::thread> threads;
    for (int t = 0; t < nThreads; ++t)
    {
        threads.push_back(std::thread([&]()
        {
            for (int r = 0; r < nRounds; ++r)
            {
                common::async::CAsyncExecutor exec(2);
                if (!exec.Start())
                {
                    nFail.fetch_add(1);
                    return;
                }

                // 顺序协程。
                for (int i = 0; i < 5; ++i)
                {
                    std::shared_ptr<CStrSeqCoro> p = exec.CoStart<CStrSeqCoro>(7);
                    if (p->Get().ValueOr(0) != 7)
                    {
                        nFail.fetch_add(1);
                    }
                }
                // 并行组。
                for (int i = 0; i < 5; ++i)
                {
                    std::shared_ptr<CStrAllCoro> p = exec.CoStart<CStrAllCoro>();
                    if (p->Get().ValueOr(0) != 36)
                    {
                        nFail.fetch_add(1);
                    }
                }
                // 任务链。
                for (int i = 0; i < 5; ++i)
                {
                    common::async::CTaskResult<int> r =
                        exec.Submit([]() { return 1; })
                            .Then([](int n) { return n + 2; })
                            .Then([](int n) { return n * 3; })
                            .Get();
                    if (r.ValueOr(0) != 9)
                    {
                        nFail.fetch_add(1);
                    }
                }

                exec.Stop();
            }
        }));
    }
    for (size_t i = 0; i < threads.size(); ++i)
    {
        threads[i].join();
    }
    ASSERT_EQ(nFail.load(), 0);
}
