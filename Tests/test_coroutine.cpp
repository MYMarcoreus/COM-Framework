/// @file test_coroutine.cpp
/// Common 无栈协程（common::async::CCoroutine）单元测试。

#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
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
        CO_AWAIT_INTO(m_nA, []() { return 3; });            // 落地值（非 void）
        CO_AWAIT_INTO(m_nB, [this]() { return m_nA * 2; }); // 捕获 this
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
        CO_AWAIT([this]() { m_nDone.fetch_add(1); }); // 纯等待（void 任务）
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
        CO_AWAIT_INTO(m_nAfter, []() -> int { throw std::runtime_error("boom"); });
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
        CO_AWAIT([this]() { m_pCounter->fetch_add(1); }); // 纯等待
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
        CO_AWAIT_INTO(m_nV, m_pExec->Submit([]() { return 3; })
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
        CO_AWAIT_INTO(*m_spResult, []() { return 42; });  // 落地值：target = *sp
        CO_AWAIT([this]() { *m_spResult += 1; });          // 纯等待
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
        CO_AWAIT_INTO(m_nV, []() { return 7; });
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
        CO_AWAIT_INTO(m_nR, m_spChild->AsTask()); // await 子协程结果。
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
        CO_AWAIT_ALL_INTO(m_nA, []() { return 1; },
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
        CO_AWAIT_ALL_INTO(m_nA, []() { return 1; },
                          m_nB, []() -> int { throw std::runtime_error("boom"); });
        CO_RETURN(m_nA + m_nB);
        CO_END();
    }

private:
    int m_nA;
    int m_nB;
};

// 主版本 CO_AWAIT 纯等待：非 void 任务忽略返回值，值经外部共享 shared_ptr 传递。
class CSharedWaitCoro : public common::async::CCoroutine<int>
{
public:
    explicit CSharedWaitCoro(const std::shared_ptr<int>& spVal) : m_spVal(spVal) {}

    void Run() override
    {
        CO_BEGIN();
        // ① 纯等待非 void 任务（返回值忽略）。
        CO_AWAIT([]() { return 5; });
        // ② 任务 lambda 捕获共享对象写结果（值经共享指针传递）。
        CO_AWAIT([this]() { *m_spVal = 42; });
        CO_RETURN(*m_spVal);
        CO_END();
    }

private:
    std::shared_ptr<int> m_spVal;
};

// 并行纯等待（CO_AWAIT_ALL 主版本）：忽略返回值，值经共享 shared_ptr 传递。
class CAllWaitCoro : public common::async::CCoroutine<int>
{
public:
    explicit CAllWaitCoro(const std::shared_ptr<std::atomic<int> >& spVal) : m_spVal(spVal) {}

    void Run() override
    {
        CO_BEGIN();
        // 并行三个任务：两个写共享原子变量，一个返回非 void（返回值忽略）。
        CO_AWAIT_ALL([this]() { m_spVal->fetch_add(1); },
                     [this]() { m_spVal->fetch_add(2); },
                     []() { return 100; });
        CO_RETURN(m_spVal->load());
        CO_END();
    }

private:
    std::shared_ptr<std::atomic<int> > m_spVal;
};

// ================================================================
// 进阶测试协程类与辅助工具
// ================================================================

/// @brief 可比较坐标点（复杂值类型测试用）。
struct CPoint
{
    int nX;
    int nY;
    CPoint() : nX(0), nY(0) {}
    CPoint(int nX_, int nY_) : nX(nX_), nY(nY_) {}
};

bool operator==(const CPoint& a, const CPoint& b)
{
    return a.nX == b.nX && a.nY == b.nY;
}

/// @brief 线程安全执行顺序记录器（单线程确定性验证用）。
class CTrace
{
public:
    void Add(const std::string& strStep)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_vecSteps.push_back(strStep);
    }

    size_t Size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_vecSteps.size();
    }

private:
    mutable std::mutex m_mutex;
    std::vector<std::string> m_vecSteps;
};

// ---- 顺序 await：三次混合（落地值 → 纯等待 → 落地值）----

/// 落地值 → 纯等待（void）→ 依赖前值的落地值。
class CTripleCoro : public common::async::CCoroutine<int>
{
public:
    CTripleCoro() : m_nA(0), m_nB(0), m_nTouched(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_INTO(m_nA, []() { return 2; });            // 落地值
        CO_AWAIT([this]() { m_nTouched.fetch_add(1); });    // 纯等待（void）
        CO_AWAIT_INTO(m_nB, [this]() { return m_nA * 3; }); // 依赖前值
        CO_RETURN(m_nA + m_nB);
        CO_END();
    }

    int Touched() const { return m_nTouched.load(); }

private:
    int m_nA;
    int m_nB;
    std::atomic<int> m_nTouched;
};

// ---- 顺序 await：已提交 CTask 链直接 await ----

/// 落地值 = await(Submit(...).Then(...).Then(...))：验证 CTask 链整体 await。
class CChainCoro : public common::async::CCoroutine<int>
{
public:
    explicit CChainCoro(common::async::CAsyncExecutor* pExec) : m_pExec(pExec), m_nV(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_INTO(m_nV, m_pExec->Submit([]() { return 3; })
                            .Then([](int n) { return n * 2; })   // 6
                            .Then([](int n) { return n + 4; })); // 10
        CO_RETURN(m_nV);
        CO_END();
    }

private:
    common::async::CAsyncExecutor* m_pExec;
    int m_nV;
};

// ---- 复杂值类型：string + struct 落地值 ----

/// 落地值支持任意可拷贝类型（std::string / 自定义 struct），且可被后续 await 依赖。
class CComplexValueCoro : public common::async::CCoroutine<std::string>
{
public:
    CComplexValueCoro() : m_pt(), m_nSum(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_INTO(m_strName, []() { return std::string("hello"); });
        CO_AWAIT_INTO(m_pt, []() { return CPoint(1, 2); });
        CO_AWAIT_INTO(m_nSum, [this]() { return m_pt.nX + m_pt.nY; }); // 依赖落地值
        CO_RETURN(m_strName + ":" + std::to_string(m_nSum));
        CO_END();
    }

    /// @brief 落地值 struct（供测试验证）。
    const CPoint& Point() const { return m_pt; }

private:
    std::string m_strName;
    CPoint m_pt;
    int m_nSum;
};

// ---- 值传递：生产者-消费者（共享 shared_ptr 槽交换数据）----

/// 生产者：写入共享槽。
class CProducerCoro : public common::async::CCoroutine<int>
{
public:
    explicit CProducerCoro(const std::shared_ptr<int>& spSlot) : m_spSlot(spSlot) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT([this]() { *m_spSlot = 100; }); // 生产
        CO_AWAIT([]() { return 0; });            // 让出
        CO_RETURN(*m_spSlot);
        CO_END();
    }

private:
    std::shared_ptr<int> m_spSlot;
};

/// 消费者：读取共享槽（数据经共享 shared_ptr 跨协程交换）。
class CConsumerCoro : public common::async::CCoroutine<int>
{
public:
    explicit CConsumerCoro(const std::shared_ptr<int>& spSlot) : m_spSlot(spSlot) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT([]() {});    // 让生产者先写
        CO_RETURN(*m_spSlot); // 消费
        CO_END();
    }

private:
    std::shared_ptr<int> m_spSlot;
};

// ---- 终止：await 到业务 None（Then 变换返回 None）----

/// Then 变换返回 None → 任务无值（kEndNone）→ 协程终止，后续不执行。
class CNoneTaskCoro : public common::async::CCoroutine<int>
{
public:
    explicit CNoneTaskCoro(common::async::CAsyncExecutor* pExec)
        : m_pExec(pExec), m_nAfter(0) {}

    void Run() override
    {
        CO_BEGIN();
        // Then 变换返回 None → 任务无值（kEndNone）→ 协程终止。
        CO_AWAIT_INTO(m_nAfter, m_pExec->Submit([]() { return 5; })
                            .Then([](int) -> common::async::CTaskResult<int>
                            {
                                return common::async::None; // 业务终止
                            }));
        CO_RETURN(m_nAfter); // 不会执行（IsTerminated 拦截）。
        CO_END();
    }

private:
    common::async::CAsyncExecutor* m_pExec;
    int m_nAfter;
};

// ---- 并行：CO_AWAIT_ALL 混合 void / 非 void ----

/// 主版本并行：void 任务写成员 + 非 void 任务（返回值忽略）。
class CMixedAllCoro : public common::async::CCoroutine<int>
{
public:
    CMixedAllCoro() : m_nA(0), m_nB(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_ALL([this]() { m_nA = 10; },  // void 任务（写成员）
                     []() { return 20; },      // 非 void（值忽略）
                     [this]() { m_nB = 30; }); // void 任务
        CO_RETURN(m_nA + m_nB);                // 40
        CO_END();
    }

private:
    int m_nA;
    int m_nB;
};

// ---- 并行：CO_AWAIT_ALL_INTO 混入已提交 CTask（含链）----

/// 并行落地值混用三种任务形态：CTask / 裸 lambda / CTask 链。
class CAllWithTaskCoro : public common::async::CCoroutine<int>
{
public:
    explicit CAllWithTaskCoro(common::async::CAsyncExecutor* pExec)
        : m_pExec(pExec), m_nA(0), m_nB(0), m_nC(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_ALL_INTO(m_nA, m_pExec->Submit([]() { return 1; }),        // CTask
                          m_nB, []() { return 2; },                         // 裸 lambda
                          m_nC, m_pExec->Submit([]() { return 3; })
                                      .Then([](int n) { return n + 1; }));  // 链
        CO_RETURN(m_nA + m_nB + m_nC); // 1 + 2 + 4 = 7
        CO_END();
    }

private:
    common::async::CAsyncExecutor* m_pExec;
    int m_nA, m_nB, m_nC;
};

// ---- 并行：8 个落地值 ----

/// 大量并行（8 路）全部落地并正确求和。
class CManyAllCoro : public common::async::CCoroutine<int>
{
public:
    CManyAllCoro()
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
        CO_RETURN(m_n1 + m_n2 + m_n3 + m_n4 + m_n5 + m_n6 + m_n7 + m_n8);
        CO_END();
    }

private:
    int m_n1, m_n2, m_n3, m_n4, m_n5, m_n6, m_n7, m_n8;
};

// ---- 并行：CO_AWAIT_ALL 含异常任务 ----

/// 并行纯等待中含异常任务 → 整体终止（kException）。
class CAllWaitNoneCoro : public common::async::CCoroutine<int>
{
public:
    CAllWaitNoneCoro() : m_nA(0), m_nB(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_ALL([this]() { m_nA = 1; },
                     []() -> int { throw std::runtime_error("boom"); },
                     [this]() { m_nB = 2; });
        CO_RETURN(m_nA + m_nB);
        CO_END();
    }

private:
    int m_nA, m_nB;
};

// ---- 并行：多线程真实计算（并行度验证）----

/// 4 路并行计算（各含 CPU 忙循环），多线程下结果正确。
class CParallelWorkCoro : public common::async::CCoroutine<int>
{
public:
    CParallelWorkCoro() : m_n1(0), m_n2(0), m_n3(0), m_n4(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_ALL_INTO(m_n1, []() { return Heavy(1); },
                          m_n2, []() { return Heavy(2); },
                          m_n3, []() { return Heavy(3); },
                          m_n4, []() { return Heavy(4); });
        CO_RETURN(m_n1 + m_n2 + m_n3 + m_n4);
        CO_END();
    }

private:
    static int Heavy(int n)
    {
        volatile int nAcc = 0;
        for (int i = 0; i < 200000; ++i)
        {
            nAcc += i * n;
        }
        return n;
    }

    int m_n1, m_n2, m_n3, m_n4;
};

// ---- 嵌套：三级（孙 → 子 → 根）----

/// 孙：产出 2。
class CGrandChildCoro : public common::async::CCoroutine<int>
{
public:
    CGrandChildCoro() : m_nV(0) {}

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

/// 子：await 孙，乘 3。
class CMidChildCoro : public common::async::CCoroutine<int>
{
public:
    explicit CMidChildCoro(const std::shared_ptr<CGrandChildCoro>& spChild)
        : m_spChild(spChild), m_nR(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_INTO(m_nR, m_spChild->AsTask()); // 2
        CO_RETURN(m_nR * 3);                      // 6
        CO_END();
    }

private:
    std::shared_ptr<CGrandChildCoro> m_spChild;
    int m_nR;
};

/// 根：await 子，乘 4。
class CRootCoro : public common::async::CCoroutine<int>
{
public:
    explicit CRootCoro(const std::shared_ptr<CMidChildCoro>& spMid)
        : m_spMid(spMid), m_nR(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_INTO(m_nR, m_spMid->AsTask()); // 6
        CO_RETURN(m_nR * 4);                    // 24
        CO_END();
    }

private:
    std::shared_ptr<CMidChildCoro> m_spMid;
    int m_nR;
};

// ---- 复用：同一协程对象二次 Start（Reset 复位）----

/// 协程对象可二次 Start：Reset 复位状态后重新执行。
class CReusableCoro : public common::async::CCoroutine<int>
{
public:
    CReusableCoro() : m_nV(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_INTO(m_nV, []() { return 5; });
        CO_RETURN(m_nV * 2);
        CO_END();
    }

private:
    int m_nV;
};

// ---- 顺序验证：恢复后记录步骤 ----

/// 恢复点后执行普通语句（宏间非 await 语句），验证状态机正确跳转。
class COrderCoro : public common::async::CCoroutine<int>
{
public:
    COrderCoro(const std::shared_ptr<CTrace>& spTrace, int nId)
        : m_spTrace(spTrace), m_nId(nId) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT([]() {});                         // 让出一次，验证恢复点
        m_spTrace->Add(std::to_string(m_nId));     // 恢复后记录（宏间普通语句）
        CO_RETURN(m_nId);
        CO_END();
    }

private:
    std::shared_ptr<CTrace> m_spTrace;
    int m_nId;
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

/// @brief 主版本 CO_AWAIT 纯等待：非 void 忽略返回值，值经共享 shared_ptr 传递。
TEST(Coroutine_SharedPtrWait)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<int> spVal = std::make_shared<int>(0);
    std::shared_ptr<CSharedWaitCoro> pCoro = exec.CoStart<CSharedWaitCoro>(spVal);
    common::async::CTaskResult<int> r = pCoro->Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 42);
    exec.Stop();
}

/// @brief 主版本 CO_AWAIT_ALL 并行纯等待：忽略返回值，值经共享 shared_ptr 传递。
TEST(Coroutine_AwaitAllWait)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<std::atomic<int> > spVal = std::make_shared<std::atomic<int> >(0);
    std::shared_ptr<CAllWaitCoro> pCoro = exec.CoStart<CAllWaitCoro>(spVal);
    common::async::CTaskResult<int> r = pCoro->Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 3); // 1 + 2（共享原子变量累加）
    exec.Stop();
}

// ================================================================
// 进阶测试
// ================================================================

/// @brief 三次混合 await（落地值 → 纯等待 → 落地值），结果与纯等待次数正确。
TEST(Coroutine_TripleAwait)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTripleCoro> pCoro = exec.CoStart<CTripleCoro>();
    common::async::CTaskResult<int> r = pCoro->Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 8);        // 2 + 2*3
    ASSERT_EQ(pCoro->Touched(), 1); // 纯等待任务执行一次
    exec.Stop();
}

/// @brief await 已提交 CTask 链（Submit → Then → Then）：结果 = 链末值。
TEST(Coroutine_AwaitTaskChain)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CChainCoro> pCoro = exec.CoStart<CChainCoro>(&exec);
    common::async::CTaskResult<int> r = pCoro->Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 10); // 3*2+4
    exec.Stop();
}

/// @brief 复杂值类型落地值（std::string / struct），且可被后续 await 依赖。
TEST(Coroutine_ComplexValues)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CComplexValueCoro> pCoro = exec.CoStart<CComplexValueCoro>();
    common::async::CTaskResult<std::string> r = pCoro->Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), std::string("hello:3"));
    ASSERT_TRUE(pCoro->Point() == CPoint(1, 2)); // struct 落地值正确
    exec.Stop();
}

/// @brief 生产者-消费者：共享 shared_ptr 槽跨协程交换数据。
TEST(Coroutine_ProducerConsumer)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<int> spSlot = std::make_shared<int>(0);
    std::shared_ptr<CConsumerCoro> pConsumer = exec.CoStart<CConsumerCoro>(spSlot);
    std::shared_ptr<CProducerCoro> pProducer = exec.CoStart<CProducerCoro>(spSlot);
    common::async::CTaskResult<int> rC = pConsumer->Get();
    common::async::CTaskResult<int> rP = pProducer->Get();
    ASSERT_TRUE(rC.HasValue());
    ASSERT_EQ(rC.Value(), 100); // 消费者读到生产者写入的值
    ASSERT_TRUE(rP.HasValue());
    ASSERT_EQ(rP.Value(), 100);
    exec.Stop();
}

/// @brief await 到业务 None（Then 变换返回 None）→ 协程以 kEndNone 终止。
TEST(Coroutine_NoneTask)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CNoneTaskCoro> pCoro = exec.CoStart<CNoneTaskCoro>(&exec);
    common::async::CTaskResult<int> r = pCoro->Get();
    ASSERT_TRUE(!r.HasValue());
    ASSERT_EQ(r.Reason(), common::async::detail::kEndNone);
    exec.Stop();
}

/// @brief 主版本 CO_AWAIT_ALL 混合 void / 非 void 任务：全部完成恢复。
TEST(Coroutine_AwaitAllMixed)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CMixedAllCoro> pCoro = exec.CoStart<CMixedAllCoro>();
    common::async::CTaskResult<int> r = pCoro->Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 40); // 10 + 30（非 void 返回值忽略）
    exec.Stop();
}

/// @brief 并行落地值混用三种任务形态（CTask / 裸 lambda / CTask 链）。
TEST(Coroutine_AwaitAllWithTask)
{
    common::async::CAsyncExecutor exec(3);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CAllWithTaskCoro> pCoro = exec.CoStart<CAllWithTaskCoro>(&exec);
    common::async::CTaskResult<int> r = pCoro->Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 7); // 1 + 2 + 4
    exec.Stop();
}

/// @brief 大量并行（8 路）全部落地并正确求和。
TEST(Coroutine_AwaitAllMany)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CManyAllCoro> pCoro = exec.CoStart<CManyAllCoro>();
    common::async::CTaskResult<int> r = pCoro->Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 36); // 1+2+...+8
    exec.Stop();
}

/// @brief 并行纯等待含异常任务 → 整体以 kException 终止。
TEST(Coroutine_AwaitAllWaitNone)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CAllWaitNoneCoro> pCoro = exec.CoStart<CAllWaitNoneCoro>();
    common::async::CTaskResult<int> r = pCoro->Get();
    ASSERT_TRUE(!r.HasValue());
    ASSERT_EQ(r.Reason(), common::async::detail::kException);
    exec.Stop();
}

/// @brief 三级嵌套协程（孙 → 子 → 根）：AsTask 逐级 await。
TEST(Coroutine_NestedThreeLevel)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CGrandChildCoro> pGrand = exec.CoStart<CGrandChildCoro>();
    std::shared_ptr<CMidChildCoro> pMid = exec.CoStart<CMidChildCoro>(pGrand);
    std::shared_ptr<CRootCoro> pRoot = exec.CoStart<CRootCoro>(pMid);
    common::async::CTaskResult<int> r = pRoot->Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 24); // 2 * 3 * 4
    exec.Stop();
}

/// @brief 同一协程对象二次 Start：Reset 复位后重新执行。
TEST(Coroutine_ReuseCoStart)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CReusableCoro> pCoro = exec.CoStart<CReusableCoro>();
    common::async::CTaskResult<int> r1 = pCoro->Get();
    ASSERT_TRUE(r1.HasValue());
    ASSERT_EQ(r1.Value(), 10); // 5*2

    pCoro->Start(&exec); // 同一对象二次启动（Reset 复位 + 重投递）
    common::async::CTaskResult<int> r2 = pCoro->Get();
    ASSERT_TRUE(r2.HasValue());
    ASSERT_EQ(r2.Value(), 10);
    exec.Stop();
}

/// @brief 执行器 Stop 后再 CoStart → 协程立即以 kStopped 终止（Post 被拒）。
TEST(Coroutine_CoStartAfterStop)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());
    exec.Stop();

    std::shared_ptr<CSeqCoro> pCoro = exec.CoStart<CSeqCoro>();
    common::async::CTaskResult<int> r = pCoro->Get();
    ASSERT_TRUE(!r.HasValue());
    ASSERT_EQ(r.Reason(), common::async::detail::kStopped);
}

/// @brief Stop 后再 Start（重建句柄隔离旧任务）：协程可正常运行。
TEST(Coroutine_StopThenRestart)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());
    exec.Stop();
    ASSERT_TRUE(exec.Start()); // 重建句柄与线程池

    std::shared_ptr<CSeqCoro> pCoro = exec.CoStart<CSeqCoro>();
    common::async::CTaskResult<int> r = pCoro->Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 9);
    exec.Stop();
}

/// @brief Get 幂等：重复调用返回相同结果。
TEST(Coroutine_GetMultiple)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CSeqCoro> pCoro = exec.CoStart<CSeqCoro>();
    common::async::CTaskResult<int> r1 = pCoro->Get();
    common::async::CTaskResult<int> r2 = pCoro->Get(); // 幂等
    ASSERT_TRUE(r1.HasValue());
    ASSERT_EQ(r1.Value(), 9);
    ASSERT_TRUE(r2.HasValue());
    ASSERT_EQ(r2.Value(), 9);
    exec.Stop();
}

/// @brief 单线程执行器：多个协程均正常完成，恢复点后的普通语句正确执行。
TEST(Coroutine_SingleThreadOrdering)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTrace> spTrace = std::make_shared<CTrace>();
    std::vector<std::shared_ptr<COrderCoro> > vCoros;
    for (int i = 0; i < 8; ++i)
    {
        vCoros.push_back(exec.CoStart<COrderCoro>(spTrace, i));
    }
    for (size_t i = 0; i < vCoros.size(); ++i)
    {
        common::async::CTaskResult<int> r = vCoros[i]->Get();
        ASSERT_TRUE(r.HasValue());
        ASSERT_EQ(r.Value(), static_cast<int>(i));
    }
    ASSERT_EQ(spTrace->Size(), 8); // 每个协程恢复后各记录一次
    exec.Stop();
}

/// @brief 压力：64 个协程（32 有值 + 32 void）并发，全部正确完成。
TEST(Coroutine_StressMany)
{
    common::async::CAsyncExecutor exec(8);
    ASSERT_TRUE(exec.Start());

    std::atomic<int> nCounter(0);
    const int nEach = 32;
    std::vector<std::shared_ptr<CIncCoro> > vVoid;
    std::vector<std::shared_ptr<CSeqCoro> > vVal;
    for (int i = 0; i < nEach; ++i)
    {
        vVoid.push_back(exec.CoStart<CIncCoro>(&nCounter));
        vVal.push_back(exec.CoStart<CSeqCoro>());
    }
    for (size_t i = 0; i < vVoid.size(); ++i)
    {
        ASSERT_TRUE(vVoid[i]->Get().HasValue());
    }
    for (size_t i = 0; i < vVal.size(); ++i)
    {
        common::async::CTaskResult<int> r = vVal[i]->Get();
        ASSERT_TRUE(r.HasValue());
        ASSERT_EQ(r.Value(), 9);
    }
    ASSERT_EQ(nCounter.load(), nEach);
    exec.Stop();
}

/// @brief 多线程并行：4 路 CPU 计算任务并行 await，结果正确。
TEST(Coroutine_MultiThreadAwaitAll)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CParallelWorkCoro> pCoro = exec.CoStart<CParallelWorkCoro>();
    common::async::CTaskResult<int> r = pCoro->Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 10); // 1+2+3+4
    exec.Stop();
}
