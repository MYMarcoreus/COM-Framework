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

// 顺序两次 await，Return 和（跨 await 变量 = 成员；裸 lambda 自动 Submit）。
class CSeqCoro : public common::async::CCoroutine<int>
{
public:
    CSeqCoro() : m_nA(0), m_nB(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(m_nA, []() { return 3; });            // 裸 lambda 自动 Submit
        CO_AWAIT(m_nB, [this]() { return m_nA * 2; }); // 捕获 this
        CO_RETURN(m_nA + m_nB);
        CO_END();
    }

private:
    int m_nA;
    int m_nB;
};

// 先 await 一个 void 任务，再 Return 值（void 裸 lambda 自动 Submit）。
class CVoidAwaitCoro : public common::async::CCoroutine<int>
{
public:
    CVoidAwaitCoro() : m_nDone(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_VOID([this]() { m_nDone.fetch_add(1); });
        CO_RETURN(m_nDone.load());
        CO_END();
    }

private:
    std::atomic<int> m_nDone;
};

// await 到异常任务 → 无值（kException），后续不执行（裸 lambda 抛异常自动 Submit）。
class CNoneCoro : public common::async::CCoroutine<int>
{
public:
    CNoneCoro() : m_nAfter(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(m_nAfter, []() -> int { throw std::runtime_error("boom"); });
        // 上游终止 → 不会执行到这里（case 处 IsTerminated() 拦截）。
        CO_RETURN(m_nAfter);
        CO_END();
    }

private:
    int m_nAfter;
};

// 无返回值协程：void 任务 + Return void。
class CIncCoro : public common::async::CCoroutine<void>
{
public:
    explicit CIncCoro(std::atomic<int>* pCounter) : m_pCounter(pCounter) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_VOID([this]() { m_pCounter->fetch_add(1); });
        CO_RETURN_VOID();
        CO_END();
    }

private:
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

// shared_ptr 数据模式：数据对象动态分配，task lambda 捕获 sp，target 用 *sp。
class CSharedPtrCoro : public common::async::CCoroutine<int>
{
public:
    CSharedPtrCoro() : m_spResult(std::make_shared<int>(0)) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(*m_spResult, []() { return 42; });  // target = *sp（解引用）
        CO_AWAIT_VOID([this]() { *m_spResult += 1; });   // void 裸 lambda
        CO_RETURN(*m_spResult);
        CO_END();
    }

    // shared_ptr 随协程常驻，数据可跨 await / 跨任务传递。
    std::shared_ptr<int> m_spResult;
};

// 子协程（嵌套 await 用）。
class CChildCoro : public common::async::CCoroutine<int>
{
public:
    CChildCoro() : m_nV(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(m_nV, []() { return 7; });
        CO_RETURN(m_nV);
        CO_END();
    }

private:
    int m_nV;
};

// 父协程：await 子协程（AsTask）。
class CParentCoro : public common::async::CCoroutine<int>
{
public:
    explicit CParentCoro(const std::shared_ptr<CChildCoro>& spChild)
        : m_spChild(spChild), m_nR(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(m_nR, m_spChild->AsTask()); // await 子协程结果。
        CO_RETURN(m_nR * 2);
        CO_END();
    }

private:
    std::shared_ptr<CChildCoro> m_spChild;
    int m_nR;
};

// 并行 await 三个任务。
class CAllCoro : public common::async::CCoroutine<int>
{
public:
    CAllCoro() : m_nA(0), m_nB(0), m_nC(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_ALL(m_nA, []() { return 1; },
                     m_nB, []() { return 2; },
                     m_nC, []() { return 3; });
        CO_RETURN(m_nA + m_nB + m_nC);
        CO_END();
    }

private:
    int m_nA;
    int m_nB;
    int m_nC;
};

// 并行 await 含异常任务 → 整体终止。
class CAllNoneCoro : public common::async::CCoroutine<int>
{
public:
    CAllNoneCoro() : m_nA(0), m_nB(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_ALL(m_nA, []() { return 1; },
                     m_nB, []() -> int { throw std::runtime_error("boom"); });
        CO_RETURN(m_nA + m_nB);
        CO_END();
    }

private:
    int m_nA;
    int m_nB;
};

} // namespace

/// @brief 顺序两次 await，结果正确（3 + 3*2 = 9）。
TEST(Coroutine_SequentialAwait)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CSeqCoro> pCoro = exec.CoStart<CSeqCoro>();
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

    std::shared_ptr<CVoidAwaitCoro> pCoro = exec.CoStart<CVoidAwaitCoro>();
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

    std::shared_ptr<CNoneCoro> pCoro = exec.CoStart<CNoneCoro>();
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
        coros.push_back(exec.CoStart<CIncCoro>(&nCounter));
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

/// @brief shared_ptr 数据模式：target 用 *sp 解引用，void 裸 lambda 传递。
TEST(Coroutine_SharedPtrData)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CSharedPtrCoro> pCoro = exec.CoStart<CSharedPtrCoro>();
    common::async::CTaskResult<int> r = pCoro->Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 43); // 42 + 1
    exec.Stop();
}

/// @brief 执行器未启动：CoStart 投递失败 → 协程安全以 kStopped 终止。
TEST(Coroutine_NotStartedStopped)
{
    common::async::CAsyncExecutor exec(1); // 未 Start。

    std::shared_ptr<CSeqCoro> pCoro = exec.CoStart<CSeqCoro>();
    common::async::CTaskResult<int> r = pCoro->Get();
    ASSERT_TRUE(!r.HasValue());
    ASSERT_EQ(r.Reason(), common::async::detail::kStopped);
}

/// @brief 用户提前释放 shared_ptr，已投递的 Resume/回调仍安全（对象延迟析构）。
TEST(Coroutine_LifetimeReleasedEarly)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    {
        // 协程 await 两次；作用域结束即释放 pCoro（不 Get）。
        std::shared_ptr<CSeqCoro> pCoro = exec.CoStart<CSeqCoro>();
    }
    exec.Stop(); // 等待所有任务完成（含协程的 Resume）。
    // 未悬垂 / 未崩溃即通过。
}

/// @brief 嵌套协程：父协程 await 子协程（AsTask）。
TEST(Coroutine_NestedAwait)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CChildCoro> pChild = exec.CoStart<CChildCoro>();
    std::shared_ptr<CParentCoro> pParent = exec.CoStart<CParentCoro>(pChild);
    common::async::CTaskResult<int> r = pParent->Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 14); // 7 * 2
    exec.Stop();
}

/// @brief 并行 await 三个任务，全部完成恢复（1+2+3=6）。
TEST(Coroutine_AwaitAll)
{
    common::async::CAsyncExecutor exec(3);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CAllCoro> pCoro = exec.CoStart<CAllCoro>();
    common::async::CTaskResult<int> r = pCoro->Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 6);
    exec.Stop();
}

/// @brief 并行 await 含异常任务 → 协程以 kException 终止。
TEST(Coroutine_AwaitAllNone)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CAllNoneCoro> pCoro = exec.CoStart<CAllNoneCoro>();
    common::async::CTaskResult<int> r = pCoro->Get();
    ASSERT_TRUE(!r.HasValue());
    ASSERT_EQ(r.Reason(), common::async::detail::kException);
    exec.Stop();
}
