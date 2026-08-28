/// @file test_perf.cpp
/// Common 异步框架性能测试（common::async 协程 / 任务链）。
///
/// 目标：测量关键路径吞吐（ops/s）并打印报告；同时验证结果正确性。
/// 断言策略：只用极宽松下限（防灾难性退化），不做精确耗时断言（避免 flaky）。
/// 所有测试应在 release（-O2）下运行以获得有意义的吞吐数据。

#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

#include "TestFramework.h"

#include "Async/AsyncExecutor.h"
#include "Async/Coroutine.h"

namespace {

// ------------------------------------------------------------------
// 性能测试用协程类
// ------------------------------------------------------------------

/// 单次 await 协程（顺序路径最小开销）。
class CPerfSeqCoro : public common::async::CCoroutine<int>
{
public:
    CPerfSeqCoro() : m_nV(0) {}

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

/// 8 次顺序 await（测恢复路径吞吐；协程内 await 严格串行，实例状态独立）。
class CPerfMultiCoro : public common::async::CCoroutine<int>
{
public:
    CPerfMultiCoro() : m_nAcc(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_INTO(m_nAcc, []() { return 1; });
        CO_AWAIT_INTO(m_nAcc, [this]() { return m_nAcc + 1; });
        CO_AWAIT_INTO(m_nAcc, [this]() { return m_nAcc + 1; });
        CO_AWAIT_INTO(m_nAcc, [this]() { return m_nAcc + 1; });
        CO_AWAIT_INTO(m_nAcc, [this]() { return m_nAcc + 1; });
        CO_AWAIT_INTO(m_nAcc, [this]() { return m_nAcc + 1; });
        CO_AWAIT_INTO(m_nAcc, [this]() { return m_nAcc + 1; });
        CO_AWAIT_INTO(m_nAcc, [this]() { return m_nAcc + 1; });
        CO_RETURN(m_nAcc); // 8
        CO_END();
    }

private:
    int m_nAcc;
};

/// 4 路并行 await（测并行组吞吐）。
class CPerfAllCoro : public common::async::CCoroutine<int>
{
public:
    CPerfAllCoro() : m_n1(0), m_n2(0), m_n3(0), m_n4(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_ALL_INTO(m_n1, []() { return 1; },
                          m_n2, []() { return 2; },
                          m_n3, []() { return 3; },
                          m_n4, []() { return 4; });
        CO_RETURN(m_n1 + m_n2 + m_n3 + m_n4); // 10
        CO_END();
    }

private:
    int m_n1, m_n2, m_n3, m_n4;
};

/// 子协程（嵌套性能测试用）。
class CPerfChildCoro : public common::async::CCoroutine<int>
{
public:
    CPerfChildCoro() : m_nV(0) {}

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

/// 父协程：await 子协程（AsTask）。
class CPerfParentCoro : public common::async::CCoroutine<int>
{
public:
    explicit CPerfParentCoro(const std::shared_ptr<CPerfChildCoro>& spChild)
        : m_spChild(spChild), m_nR(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_INTO(m_nR, m_spChild->AsTask());
        CO_RETURN(m_nR * 2); // 4
        CO_END();
    }

private:
    std::shared_ptr<CPerfChildCoro> m_spChild;
    int m_nR;
};

/// 协程版 3 步链（与 CTask::Then 链对比）。
class CPerfChainCoro : public common::async::CCoroutine<int>
{
public:
    CPerfChainCoro() : m_nV(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_INTO(m_nV, []() { return 3; });
        CO_AWAIT_INTO(m_nV, [this]() { return m_nV * 2; });
        CO_AWAIT_INTO(m_nV, [this]() { return m_nV + 1; });
        CO_RETURN(m_nV); // 7
        CO_END();
    }

private:
    int m_nV;
};

// ------------------------------------------------------------------
// 计时辅助
// ------------------------------------------------------------------

/// @brief 执行被测操作并打印吞吐报告。
///
/// @param szName 报告名称。
/// @param nOps 操作数（用于计算速率）。
/// @param fn 被测操作（无参数）。
/// @return 吞吐（ops/s）。
template <typename TFn>
double RunTimed(const char* szName, long nOps, TFn fn)
{
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    fn();
    std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
    double dMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double dRate = (dMs > 0.0) ? (static_cast<double>(nOps) * 1000.0 / dMs) : 0.0;
    std::printf("[PERF ] %-30s %8ld ops  %10.1f ms  -> %12.0f ops/s\n",
                szName, nOps, dMs, dRate);
    return dRate;
}

} // namespace

// ------------------------------------------------------------------
// 性能测试
// ------------------------------------------------------------------

/// @brief 基线：批量 Submit + 批量 Get（无协程）吞吐，作为协程开销对比基准。
/// 与协程测试同方法（批量提交 → 并发完成 → 逐个取结果），保证对比公平。
TEST(Perf_SubmitBaseline)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    const long nOps = 50000;
    std::vector<common::async::CTask<int> > vTasks;
    vTasks.reserve(static_cast<size_t>(nOps));
    double dRate = RunTimed("Submit+Get baseline", nOps, [&]()
    {
        for (long i = 0; i < nOps; ++i)
        {
            vTasks.push_back(exec.Submit(
                [i]() { return static_cast<int>(i % 7); }));
        }
        for (long i = 0; i < nOps; ++i)
        {
            common::async::CTaskResult<int> r = vTasks[i].Get();
            ASSERT_TRUE(r.HasValue());
        }
    });
    ASSERT_TRUE(dRate > 1000.0); // 极宽松下限：防灾难性退化。
    exec.Stop();
}

/// @brief 协程创建 + 单次 await + 完成 的吞吐。
TEST(Perf_SequentialAwait)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    const long nOps = 20000;
    std::vector<std::shared_ptr<CPerfSeqCoro> > vCoros;
    vCoros.reserve(static_cast<size_t>(nOps));
    double dRate = RunTimed("CoStart+Get (1 await)", nOps, [&]()
    {
        for (long i = 0; i < nOps; ++i)
        {
            vCoros.push_back(exec.CoStart<CPerfSeqCoro>());
        }
        for (long i = 0; i < nOps; ++i)
        {
            common::async::CTaskResult<int> r = vCoros[i]->Get();
            ASSERT_TRUE(r.HasValue());
            ASSERT_EQ(r.Value(), 7);
        }
    });
    ASSERT_TRUE(dRate > 100.0);
    exec.Stop();
}

/// @brief 每个协程 8 次顺序 await（恢复路径吞吐）。
TEST(Perf_MultiAwait)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    const long nOps = 5000; // 每个 8 次 await = 40000 次恢复。
    std::vector<std::shared_ptr<CPerfMultiCoro> > vCoros;
    vCoros.reserve(static_cast<size_t>(nOps));
    double dRate = RunTimed("CoStart+Get (8 await)", nOps, [&]()
    {
        for (long i = 0; i < nOps; ++i)
        {
            vCoros.push_back(exec.CoStart<CPerfMultiCoro>());
        }
        for (long i = 0; i < nOps; ++i)
        {
            common::async::CTaskResult<int> r = vCoros[i]->Get();
            ASSERT_TRUE(r.HasValue());
            ASSERT_EQ(r.Value(), 8);
        }
    });
    ASSERT_TRUE(dRate > 100.0);
    exec.Stop();
}

/// @brief 4 路并行 await（并行组吞吐）。
TEST(Perf_AwaitAll)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    const long nOps = 5000; // 每组 4 路 = 20000 个子任务。
    std::vector<std::shared_ptr<CPerfAllCoro> > vCoros;
    vCoros.reserve(static_cast<size_t>(nOps));
    double dRate = RunTimed("CoStart+Get (4-way ALL)", nOps, [&]()
    {
        for (long i = 0; i < nOps; ++i)
        {
            vCoros.push_back(exec.CoStart<CPerfAllCoro>());
        }
        for (long i = 0; i < nOps; ++i)
        {
            common::async::CTaskResult<int> r = vCoros[i]->Get();
            ASSERT_TRUE(r.HasValue());
            ASSERT_EQ(r.Value(), 10);
        }
    });
    ASSERT_TRUE(dRate > 100.0);
    exec.Stop();
}

/// @brief 嵌套协程（父 await 子 AsTask）吞吐。
TEST(Perf_Nested)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    const long nOps = 10000;
    std::vector<std::shared_ptr<CPerfParentCoro> > vParents;
    std::vector<std::shared_ptr<CPerfChildCoro> > vChildren;
    vParents.reserve(static_cast<size_t>(nOps));
    vChildren.reserve(static_cast<size_t>(nOps));
    double dRate = RunTimed("CoStart+Get (nested child)", nOps, [&]()
    {
        for (long i = 0; i < nOps; ++i)
        {
            vChildren.push_back(exec.CoStart<CPerfChildCoro>());
            vParents.push_back(exec.CoStart<CPerfParentCoro>(vChildren.back()));
        }
        for (long i = 0; i < nOps; ++i)
        {
            common::async::CTaskResult<int> r = vParents[i]->Get();
            ASSERT_TRUE(r.HasValue());
            ASSERT_EQ(r.Value(), 4);
        }
    });
    ASSERT_TRUE(dRate > 100.0);
    exec.Stop();
}

/// @brief CTask::Then 链 vs 协程完成同等 3 步工作，报告比率。
///
/// 说明：协程每步 await 都 Submit 一个新任务（含调度开销），而 CTask 链的
/// Then 是纯续接（不额外入池），故协程吞吐低于链属正常，比率反映架构开销。
TEST(Perf_ChainVsCoroutine)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    const long nOps = 20000;
    std::vector<common::async::CTask<int> > vTasks;
    vTasks.reserve(static_cast<size_t>(nOps));
    double dChain = RunTimed("CTask::Then chain (3-step)", nOps, [&]()
    {
        for (long i = 0; i < nOps; ++i)
        {
            vTasks.push_back(exec.Submit([]() { return 3; })
                                 .Then([](int n) { return n * 2; })
                                 .Then([](int n) { return n + 1; }));
        }
        for (long i = 0; i < nOps; ++i)
        {
            common::async::CTaskResult<int> r = vTasks[i].Get();
            ASSERT_TRUE(r.HasValue());
            ASSERT_EQ(r.Value(), 7);
        }
    });

    std::vector<std::shared_ptr<CPerfChainCoro> > vCoros;
    vCoros.reserve(static_cast<size_t>(nOps));
    double dCoro = RunTimed("CCoroutine (3-step)", nOps, [&]()
    {
        for (long i = 0; i < nOps; ++i)
        {
            vCoros.push_back(exec.CoStart<CPerfChainCoro>());
        }
        for (long i = 0; i < nOps; ++i)
        {
            common::async::CTaskResult<int> r = vCoros[i]->Get();
            ASSERT_TRUE(r.HasValue());
            ASSERT_EQ(r.Value(), 7);
        }
    });

    double dRatio = (dChain > 0.0) ? (dCoro / dChain) : 0.0;
    std::printf("[PERF ] %-30s coroutine/chain = %.3f\n",
                "coroutine vs chain ratio", dRatio);
    ASSERT_TRUE(dCoro > 100.0);
    exec.Stop();
}

/// @brief 压力：3000 个协程并发完成（单次 await），结果正确性 + 吞吐。
TEST(Perf_Stress)
{
    common::async::CAsyncExecutor exec(8);
    ASSERT_TRUE(exec.Start());

    const long nOps = 3000;
    double dRate = RunTimed("Stress 3000 coroutines", nOps, [&]()
    {
        std::vector<std::shared_ptr<CPerfSeqCoro> > vCoros;
        vCoros.reserve(static_cast<size_t>(nOps));
        for (long i = 0; i < nOps; ++i)
        {
            vCoros.push_back(exec.CoStart<CPerfSeqCoro>());
        }
        long nSum = 0;
        for (long i = 0; i < nOps; ++i)
        {
            common::async::CTaskResult<int> r = vCoros[i]->Get();
            ASSERT_TRUE(r.HasValue());
            nSum += r.Value();
        }
        ASSERT_EQ(nSum, 7 * nOps);
    });
    ASSERT_TRUE(dRate > 100.0);
    exec.Stop();
}
