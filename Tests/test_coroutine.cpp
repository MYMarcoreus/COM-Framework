/// @file test_coroutine.cpp
/// Common 无栈协程（common::async::CCoroutine）单元测试。

#include <atomic>
#include <memory>
#include <stdexcept>
#include <vector>

#include "TestFramework.h"

#include "Async/AsyncExecutor.h"
#include "Async/Coroutine.h"

namespace {

// 顺序两次 await，Return 和（跨 await 变量 = 成员）。
class CSeqCoro : public common::async::CCoroutine<int>
{
public:
    explicit CSeqCoro(common::async::CAsyncExecutor* pExec) : m_pExec(pExec), m_nA(0), m_nB(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(m_nA, m_pExec->Submit([]() { return 3; }));
        CO_AWAIT(m_nB, m_pExec->Submit([this]() { return m_nA * 2; }));
        CO_RETURN(m_nA + m_nB);
        CO_END();
    }

private:
    common::async::CAsyncExecutor* m_pExec;
    int m_nA;
    int m_nB;
};

// 先 await 一个 void 任务，再 Return 值。
class CVoidAwaitCoro : public common::async::CCoroutine<int>
{
public:
    explicit CVoidAwaitCoro(common::async::CAsyncExecutor* pExec)
        : m_pExec(pExec), m_nDone(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_VOID(m_pExec->Submit([this]() { m_nDone.fetch_add(1); }));
        CO_RETURN(m_nDone.load());
        CO_END();
    }

private:
    common::async::CAsyncExecutor* m_pExec;
    std::atomic<int> m_nDone;
};

// await 到异常任务 → 无值（kException），后续不执行。
class CNoneCoro : public common::async::CCoroutine<int>
{
public:
    explicit CNoneCoro(common::async::CAsyncExecutor* pExec) : m_pExec(pExec), m_nAfter(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(m_nAfter, m_pExec->Submit([]() -> int { throw std::runtime_error("boom"); }));
        // 上游终止 → 不会执行到这里（case 处 IsTerminated() 拦截）。
        CO_RETURN(m_nAfter);
        CO_END();
    }

private:
    common::async::CAsyncExecutor* m_pExec;
    int m_nAfter;
};

// 无返回值协程：void 任务 + Return void。
class CIncCoro : public common::async::CCoroutine<void>
{
public:
    CIncCoro(common::async::CAsyncExecutor* pExec, std::atomic<int>* pCounter)
        : m_pExec(pExec), m_pCounter(pCounter) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_VOID(m_pExec->Submit([this]() { m_pCounter->fetch_add(1); }));
        CO_RETURN_VOID();
        CO_END();
    }

private:
    common::async::CAsyncExecutor* m_pExec;
    std::atomic<int>* m_pCounter;
};

// 链式 flatMap（await 内部任务）：验证与 CTask::Then 平铺语义一致。
class CFlatMapCoro : public common::async::CCoroutine<int>
{
public:
    explicit CFlatMapCoro(common::async::CAsyncExecutor* pExec) : m_pExec(pExec), m_nV(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(m_nV, m_pExec->Submit([]() { return 3; })
                            .Then([this](int n) { return m_pExec->Submit([n]() { return n * 10; }); })
                            .Then([](int n) { return n + 5; }));
        CO_RETURN(m_nV);
        CO_END();
    }

private:
    common::async::CAsyncExecutor* m_pExec;
    int m_nV;
};

} // namespace

/// @brief 顺序两次 await，结果正确（3 + 3*2 = 9）。
TEST(Coroutine_SequentialAwait)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CSeqCoro> pCoro = exec.CoStart<CSeqCoro>(&exec);
    common::async::CTaskResult<int> r = pCoro->Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 9);
    exec.Stop();
}

/// @brief await 一个 void 任务后再 Return。
TEST(Coroutine_VoidAwait)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CVoidAwaitCoro> pCoro = exec.CoStart<CVoidAwaitCoro>(&exec);
    common::async::CTaskResult<int> r = pCoro->Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 1);
    exec.Stop();
}

/// @brief await 到异常任务 → 协程以 kException 终止，后续不执行。
TEST(Coroutine_ExceptionToNone)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CNoneCoro> pCoro = exec.CoStart<CNoneCoro>(&exec);
    common::async::CTaskResult<int> r = pCoro->Get();
    ASSERT_TRUE(!r.HasValue());
    ASSERT_EQ(r.Reason(), common::async::detail::kException);
    exec.Stop();
}

/// @brief 多个协程并发执行，全部完成（void 协程）。
TEST(Coroutine_Concurrent)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    std::atomic<int> nCounter(0);
    const int nCount = 16;
    std::vector<std::shared_ptr<CIncCoro> > coros;
    for (int i = 0; i < nCount; ++i)
    {
        coros.push_back(exec.CoStart<CIncCoro>(&exec, &nCounter));
    }
    for (int i = 0; i < nCount; ++i)
    {
        common::async::CTaskResult<void> r = coros[i]->Get();
        ASSERT_TRUE(r.HasValue());
    }
    ASSERT_EQ(nCounter.load(), nCount);
    exec.Stop();
}

/// @brief await 内部任务链（flatMap）：与 CTask::Then 平铺语义一致。
TEST(Coroutine_FlatMap)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CFlatMapCoro> pCoro = exec.CoStart<CFlatMapCoro>(&exec);
    common::async::CTaskResult<int> r = pCoro->Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 35); // 3*10 + 5
    exec.Stop();
}

/// @brief 执行器未启动：CoStart 投递失败 → 协程安全以 kStopped 终止。
TEST(Coroutine_NotStartedStopped)
{
    common::async::CAsyncExecutor exec(1); // 未 Start。

    std::shared_ptr<CSeqCoro> pCoro = exec.CoStart<CSeqCoro>(&exec);
    common::async::CTaskResult<int> r = pCoro->Get();
    ASSERT_TRUE(!r.HasValue());
    ASSERT_EQ(r.Reason(), common::async::detail::kStopped);
}
