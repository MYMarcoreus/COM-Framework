/// @file test_async_chain.cpp
/// 异步 promise（common::async 特化版）单元测试：promise + 协程。
///
/// 被测契约：
///  - 层与层之间只传「兑现 / 拒绝」（CPromiseResult），不传任意值；
///  - 数据一律走共享上下文 std::shared_ptr<TContext>（整条 promise 链共用同一实例）；
///  - 处理器签名固定：CPromiseResult(CPromiseResult, const std::shared_ptr<TContext>&)；
///  - then：失败即停；catch：仅被拒绝时执行（Resolve() 即恢复）；
///    finally：无论成败都执行且不改结果（与 JS 的 finally 一致）；
///  - 拒绝结果（携带标准异常）透传到 Await() 与 OnSettled；处理器内异常原样成为本层拒绝。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "AsyncTestKit.h"
#include "Coroutine/Coroutine.h"
#include "TestFramework.h"

// ==================== 测试用共享上下文与层函数 ====================

/// @brief 测试流程的共享上下文（链内所有层共用同一实例）。
struct CTestContext
{
    int nValue;                ///< 逐层累加的值。
    int nSteps;                ///< 已执行的层数（不含被跳过 / 未执行的层）。
    int nCatchRuns;            ///< catch 层执行次数。
    int nSeenOk;               ///< 观察到「上一层成功」的次数。
    int nSeenFailed;           ///< 观察到「上一层失败」的次数。
    std::string strFailText;   ///< 拒绝原因（异常描述；空 = 不失败）。
    std::thread::id workerId;  ///< 最后一个执行层的工作线程 id。
    std::string strTrace;      ///< 层执行轨迹（每层一个字符）。

    // 分叉用例专用：两条分支并发跑，按「共用上下文只写不同字段」的契约各写自己的一格。
    std::atomic<int> nForkA;  ///< 分叉分支 A 的执行次数。
    std::atomic<int> nForkB;  ///< 分叉分支 B 的执行次数。

    // 并行 await 用例专用：同上契约（两条子链在同一上下文上「并发」执行）。
    std::atomic<int> nParA;  ///< 并行分支 A 的执行次数。
    std::atomic<int> nParB;  ///< 并行分支 B 的执行次数。

    CTestContext()
        : nValue(0), nSteps(0), nCatchRuns(0), nSeenOk(0), nSeenFailed(0), strFailText(), nForkA(0), nForkB(0), nParA(0), nParB(0)
    {}
};

/// 层：值 +1。
static common::async::CPromiseResult StepAdd1(const std::shared_ptr<CTestContext>& spCtx)
{
    ++spCtx->nValue;
    ++spCtx->nSteps;
    spCtx->workerId = std::this_thread::get_id();
    spCtx->strTrace += "1";
    return common::async::CPromiseResult::Resolve();
}

/// 层：值 +10。
static common::async::CPromiseResult StepAdd10(const std::shared_ptr<CTestContext>& spCtx)
{
    spCtx->nValue += 10;
    ++spCtx->nSteps;
    spCtx->workerId = std::this_thread::get_id();
    spCtx->strTrace += "2";
    return common::async::CPromiseResult::Resolve();
}

/// 分叉用例专用层：分支 A 只写自己的字段（不碰分支 B 也会写的字段）。
static common::async::CPromiseResult StepForkA(const std::shared_ptr<CTestContext>& spCtx)
{
    spCtx->nForkA.fetch_add(1);
    return common::async::CPromiseResult::Resolve();
}

/// 分叉用例专用层：分支 B 只写自己的字段。
static common::async::CPromiseResult StepForkB(const std::shared_ptr<CTestContext>& spCtx)
{
    spCtx->nForkB.fetch_add(1);
    return common::async::CPromiseResult::Resolve();
}

/// 并行 await 用例专用层：分支 A 只写自己的字段（两条子链在同一上下文上并发执行，
/// 若都去写 `nValue` / `strTrace` 就是真实数据竞争 —— 见 Promise.h 文件头「并发注意」）。
static common::async::CPromiseResult StepParA(const std::shared_ptr<CTestContext>& spCtx)
{
    spCtx->nParA.fetch_add(1);
    return common::async::CPromiseResult::Resolve();
}

/// 并行 await 用例专用层：分支 B 只写自己的字段。
static common::async::CPromiseResult StepParB(const std::shared_ptr<CTestContext>& spCtx)
{
    spCtx->nParB.fetch_add(1);
    return common::async::CPromiseResult::Resolve();
}

/// 层：按上下文里的文案制造失败。
static common::async::CPromiseResult StepFail(const std::shared_ptr<CTestContext>& spCtx)
{
    ++spCtx->nSteps;
    spCtx->strTrace += "F";
    return common::async::CPromiseResult::Reject(std::runtime_error(spCtx->strFailText));
}

/// 层：被调用即留下痕迹（用于验证失败后不再执行）。
static common::async::CPromiseResult StepShouldNotRun(const std::shared_ptr<CTestContext>& spCtx)
{
    ++spCtx->nSteps;
    spCtx->strTrace += "X";
    return common::async::CPromiseResult::Resolve();
}

/// 层：抛异常（验证框架捕获 → 异常原样成为本层拒绝，不向调用方抛出）。
static common::async::CPromiseResult StepThrow(const std::shared_ptr<CTestContext>& /*spCtx*/)
{
    throw std::runtime_error("chain step boom");
}

/// 层（catch）：观察上一层拒绝并透传（回滚 / 清理的典型写法）。
static common::async::CPromiseResult StepCatchObserve(
    common::async::CPromiseResult upResult, const std::shared_ptr<CTestContext>& spCtx)
{
    ++spCtx->nCatchRuns;
    if (upResult.IsFulfilled())
    {
        ++spCtx->nSeenOk;
    }
    else
    {
        ++spCtx->nSeenFailed;
    }
    spCtx->strTrace += "A";
    return upResult;  // 透传：拒绝继续拒绝，兑现继续兑现。
}

/// 层（catch）：吞掉拒绝并恢复链（后续 then 层会重新执行）。
static common::async::CPromiseResult StepCatchRecover(
    common::async::CPromiseResult /*upStep*/, const std::shared_ptr<CTestContext>& spCtx)
{
    ++spCtx->nCatchRuns;
    spCtx->strTrace += "R";
    return common::async::CPromiseResult::Resolve();
}

// ==================== 契约与基本链路 ====================

/// @brief 处理器签名契约：then 层带不到上游结果，catch / finally 层才看得到。
TEST(Promise_ThenHandlerContract)
{
    // then 处理器：CPromiseResult(const std::shared_ptr<TContext>&) —— 拒绝不会进 then 层。
    common::async::CPromise<CTestContext>::ThenHandler fnStep = &StepAdd1;
    ASSERT_TRUE(static_cast<bool>(fnStep));

    // catch / finally 处理器：CPromiseResult(CPromiseResult, const std::shared_ptr<TContext>&)。
    common::async::CPromise<CTestContext>::ResultHandler fnResult = &StepCatchObserve;
    ASSERT_TRUE(static_cast<bool>(fnResult));

    // settled 通知：void(CPromiseResult)（只读结果，不改结果）。
    common::async::SettledNotice fnSettled = [](common::async::CPromiseResult)
    {
    };
    ASSERT_TRUE(static_cast<bool>(fnSettled));

    // 拒绝就是「携带一个标准异常」：兑现 / 拒绝只看 IsFulfilled()，结果里没有任何错误码。
    ASSERT_TRUE(common::async::CPromiseResult::Resolve().IsFulfilled());
    ASSERT_TRUE(common::async::CPromiseResult::Reject(std::runtime_error("业务拒绝：库存不足")).IsRejected());
    ASSERT_EQ(common::async::CPromiseResult::Reject(std::runtime_error("业务拒绝：库存不足")).Message(),
        std::string("业务拒绝：库存不足"));                                   // what() 原样保留
    ASSERT_TRUE(common::async::CPromiseResult::Resolve().Message().empty());  // 兑现：没有异常
    // 框架侧拒绝：同样是标准异常，只是文案固定（预建）。
    common::async::CAsyncExecutor execNotStarted(1);  // 故意不 Start：收不了任务 → 首层以框架侧拒绝收口。
    const common::async::CPromiseResult rStopped =
        execNotStarted.NewPromise(std::make_shared<CTestContext>(), &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC)
            .Await();
    ASSERT_TRUE(asynctest::IsStoppedFailure(rStopped));
    ASSERT_TRUE(!rStopped.Message().empty());
}

/// @brief 测试用的业务异常类型（带自己的种类枚举：想看「按类型分流」怎么写就看它）。
class CTestBizError : public std::runtime_error
{
public:
    /// @brief 失败种类（业务自己的概念）。
    enum EKind
    {
        kShortage,  ///< 库存不足。
        kTimeout    ///< 业务超时。
    };

    /// @brief 构造。
    ///
    /// @param eKind 种类。
    /// @param strWhat 描述。
    CTestBizError(EKind eKind, const std::string& strWhat) : std::runtime_error(strWhat), m_eKind(eKind)
    {}

    /// @brief 种类。
    ///
    /// @return 种类。
    EKind Kind() const
    {
        return m_eKind;
    }

private:
    EKind m_eKind;  ///< 种类。
};

/// 层：「构造」一个自定义异常类拒绝（保型路径）。
static common::async::CPromiseResult StepRejectCustomBuilt(const std::shared_ptr<CTestContext>& /*spCtx*/)
{
    return common::async::CPromiseResult::Reject(CTestBizError(CTestBizError::kShortage, "自定义异常的文案"));
}

/// 层：「throw」 同一个自定义异常（文本保留、类型降级路径）。
static common::async::CPromiseResult StepThrowCustom(const std::shared_ptr<CTestContext>& /*spCtx*/)
{
    throw CTestBizError(CTestBizError::kShortage, "自定义异常的文案");
}

/// @brief 类型分流与「构造保型 / throw 降级」的边界（`shared_ptr` 存储的直接后果）。
///
/// 结果里存的是「自有的」共享异常对象，所以：
///  - `Reject(CMyError(...))` 「构造」出来的拒绝 → 类型完整保留，`dynamic_cast` 能命中；
///  - 层里 `throw CMyError(...)` 出去的异常 → 框架在 `catch` 里只能按「静态类型」重建
///    （`Reject(std::runtime_error(e.what()))`），
///    文本完整保留，但动态类型降级为 `std::runtime_error`。
/// 要保证类型可分流，请在层里 `return CPromiseResult::Reject(CMyError(...))`。
TEST(Promise_ExceptionTypeFidelity)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());
    const std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();

    // ① 构造路径：类型保真（分流靠 dynamic_cast，不走 catch、不抛不捕）。
    const common::async::CPromiseResult rBuilt =
        exec.NewPromise(spCtx, &StepRejectCustomBuilt, common::async::TaskKind::kWrite, ASYNC_LOC).Await();
    ASSERT_TRUE(rBuilt.IsRejected());
    ASSERT_EQ(rBuilt.Message(), std::string("自定义异常的文案"));
    const CTestBizError* pBuiltError = dynamic_cast<const CTestBizError*>(rBuilt.Exception().get());
    ASSERT_TRUE(pBuiltError != nullptr);
    ASSERT_TRUE(pBuiltError->Kind() == CTestBizError::kShortage);

    // ② throw 路径：文本照旧，但类型只剩 runtime_error。
    const common::async::CPromiseResult rThrown =
        exec.NewPromise(spCtx, &StepThrowCustom, common::async::TaskKind::kWrite, ASYNC_LOC).Await();
    ASSERT_TRUE(rThrown.IsRejected());
    ASSERT_EQ(rThrown.Message(), std::string("自定义异常的文案"));
    ASSERT_TRUE(dynamic_cast<const CTestBizError*>(rThrown.Exception().get()) == nullptr);
    ASSERT_TRUE(dynamic_cast<const std::runtime_error*>(rThrown.Exception().get()) != nullptr);

    // ③ 兑现：没有异常对象。
    ASSERT_TRUE(common::async::CPromiseResult::Resolve().Exception() == nullptr);
    exec.Stop();
}

/// @brief 单层 promise：NewPromise → Await（兑现）。
TEST(Promise_NewPromiseAndAwait)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    common::async::CPromise<CTestContext> chain = exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC);
    const common::async::CPromiseResult r = chain.Await();

    ASSERT_TRUE(r.IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 1);
    ASSERT_EQ(spCtx->nSteps, 1);
    exec.Stop();
}

/// @brief 多层链顺序执行：数据经共享上下文传递，层间只传成败。
TEST(Promise_ThenSequence)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    common::async::CPromise<CTestContext> tail = exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                     .Then(&StepAdd10, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                     .Then(&StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC);
    const common::async::CPromiseResult r = tail.Await();

    ASSERT_TRUE(r.IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 12);  // 1 + 10 + 1
    ASSERT_EQ(spCtx->nSteps, 3);
    ASSERT_EQ(spCtx->strTrace, std::string("121"));  // 顺序确定：1 → 2 → 1
    exec.Stop();
}

/// @brief 上下文由调用方强制传入：先备好数据再起链，链内各层共用同一实例。
TEST(Promise_ContextPreparedBeforeChain)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    spCtx->nValue = 100;  // 起链前就把初始数据备好（框架不代建上下文）

    common::async::CPromise<CTestContext> chain = exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC);
    ASSERT_TRUE(chain.GetContext() == spCtx);  // 恒非空，且就是传入的那个实例

    common::async::CPromise<CTestContext> tail = chain.Then(&StepAdd10, common::async::TaskKind::kWrite, ASYNC_LOC);
    ASSERT_TRUE(tail.Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 111);
    exec.Stop();
}

/// @brief 外部注入上下文：链内所有层共用外部实例。
TEST(Promise_ExternalContext)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    common::async::CPromise<CTestContext> chain = exec.NewPromise(spCtx, &StepAdd10, common::async::TaskKind::kWrite, ASYNC_LOC);
    ASSERT_TRUE(chain.GetContext() == spCtx);  // 同一实例，不做拷贝

    ASSERT_TRUE(chain.Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 10);
    exec.Stop();
}

// ==================== 失败语义 ====================

/// @brief 失败即停（then）：失败层之后的 then 层不执行，拒绝码透传到 Await()。
TEST(Promise_ThenFailFast)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    spCtx->strFailText = "业务拒绝（失败即停）";

    common::async::CPromise<CTestContext> tail =
        exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC)
            .Then(&StepFail, common::async::TaskKind::kWrite, ASYNC_LOC)
            .Then(&StepShouldNotRun, common::async::TaskKind::kWrite, ASYNC_LOC);  // 不执行
    const common::async::CPromiseResult r = tail.Await();

    ASSERT_TRUE(r.IsRejected());
    ASSERT_EQ(r.Message(), spCtx->strFailText);     // 异常原样透传（框架不解释）
    ASSERT_EQ(spCtx->nValue, 1);                    // 只跑了第一层
    ASSERT_EQ(spCtx->strTrace, std::string("1F"));  // 没有 "X"
    exec.Stop();
}

/// @brief 处理器内异常 → 本层以「原样透传的异常」收口，框架不向调用方抛出。
TEST(Promise_ExceptionRejects)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    common::async::CPromise<CTestContext> tail = exec.NewPromise(spCtx, &StepThrow, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                     .Then(&StepShouldNotRun, common::async::TaskKind::kWrite, ASYNC_LOC);
    const common::async::CPromiseResult r = tail.Await();

    ASSERT_TRUE(r.IsRejected());
    ASSERT_EQ(r.Message(), std::string("chain step boom"));  // 异常原样成为拒绝
    ASSERT_EQ(spCtx->nSteps, 0);                             // 异常层与后续层都没留下业务痕迹
    exec.Stop();
}

/// @brief catch：上一层被拒绝时执行，upResult 携带拒绝结果（可回滚），返回 upResult 继续透传。
TEST(Promise_CatchSeesRejection)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    spCtx->strFailText = "业务拒绝（catch 观察）";

    common::async::CPromise<CTestContext> tail =
        exec.NewPromise(spCtx, &StepFail, common::async::TaskKind::kWrite, ASYNC_LOC)
            .Catch(&StepCatchObserve, common::async::TaskKind::kWrite, ASYNC_LOC)
            .Then(&StepShouldNotRun, common::async::TaskKind::kWrite, ASYNC_LOC);  // 失败仍在，不执行
    const common::async::CPromiseResult r = tail.Await();

    ASSERT_TRUE(r.IsRejected());
    ASSERT_EQ(r.Message(), spCtx->strFailText);
    ASSERT_EQ(spCtx->nCatchRuns, 1);
    ASSERT_EQ(spCtx->nSeenFailed, 1);  // 真的看到了上一层的失败
    ASSERT_EQ(spCtx->nSeenOk, 0);
    ASSERT_EQ(spCtx->strTrace, std::string("FA"));  // 被拒绝层 + catch 层
    exec.Stop();
}

/// @brief catch 返回 Resolve() → 吞掉拒绝，promise 从本层之后继续执行。
TEST(Promise_CatchRecover)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    spCtx->strFailText = "业务拒绝（catch 恢复）";

    common::async::CPromise<CTestContext> tail =
        exec.NewPromise(spCtx, &StepFail, common::async::TaskKind::kWrite, ASYNC_LOC)
            .Catch(&StepCatchRecover, common::async::TaskKind::kWrite, ASYNC_LOC)
            .Then(&StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC);  // 失败已被吞掉 → 执行
    const common::async::CPromiseResult r = tail.Await();

    ASSERT_TRUE(r.IsFulfilled());  // 链恢复
    ASSERT_EQ(spCtx->nCatchRuns, 1);
    ASSERT_EQ(spCtx->nValue, 1);
    ASSERT_EQ(spCtx->strTrace, std::string("FR1"));  // 失败 → 恢复 → 继续
    exec.Stop();
}

/// @brief OnSettled：兑现与拒绝都触发一次（携带最终结果）。
TEST(Promise_OnSettledCallback)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    // 成功链
    std::shared_ptr<CTestContext> spOk = std::make_shared<CTestContext>();
    std::atomic<int> nOk(0);
    std::atomic<bool> bOkDone(false);
    common::async::CPromise<CTestContext> chainOk = exec.NewPromise(spOk, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                        .Then(&StepAdd10, common::async::TaskKind::kWrite, ASYNC_LOC);
    chainOk.OnSettled(
        [&nOk, &bOkDone](common::async::CPromiseResult r)
        {
            nOk.store(r.IsFulfilled() ? 1 : -1);  // 兑现：结果里没有异常
            bOkDone.store(true);
        });
    chainOk.Await();
    while (!bOkDone.load())
    {
        std::this_thread::yield();
    }
    ASSERT_EQ(nOk.load(), 1);  // 兑现

    // 失败链
    std::shared_ptr<CTestContext> spFail = std::make_shared<CTestContext>();
    spFail->strFailText = "业务拒绝（OnSettled 用例）";
    std::atomic<bool> bSawText(false);
    std::atomic<bool> bFailDone(false);
    common::async::CPromise<CTestContext> chainFail =
        exec.NewPromise(spFail, &StepFail, common::async::TaskKind::kWrite, ASYNC_LOC);
    chainFail.OnSettled(
        [&bSawText, &bFailDone, spFail](common::async::CPromiseResult r)
        {
            bSawText.store(r.Message() == spFail->strFailText);  // 拒绝：异常描述跟着结果走
            bFailDone.store(true);
        });
    chainFail.Await();
    while (!bFailDone.load())
    {
        std::this_thread::yield();
    }
    ASSERT_TRUE(bSawText.load());
    exec.Stop();
}

// ==================== 注册时机与分叉 ====================

/// @brief 分叉：同一层注册两个 Then，各自独立延续。
TEST(Promise_Fork)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    common::async::CPromise<CTestContext> head = exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC);

    std::atomic<int> nDone(0);
    common::async::CPromise<CTestContext> branchA = head.Then(&StepForkA, common::async::TaskKind::kWrite, ASYNC_LOC);
    common::async::CPromise<CTestContext> branchB = head.Then(&StepForkB, common::async::TaskKind::kWrite, ASYNC_LOC);
    ASSERT_TRUE(branchA.GetContext() == spCtx);  // 三条链（首层 + 两条分支）共用同一上下文实例
    ASSERT_TRUE(branchB.GetContext() == spCtx);
    branchA.OnSettled(
        [&nDone](common::async::CPromiseResult)
        {
            nDone.fetch_add(1);
        });
    branchB.OnSettled(
        [&nDone](common::async::CPromiseResult)
        {
            nDone.fetch_add(1);
        });

    ASSERT_TRUE(branchA.Await().IsFulfilled());
    ASSERT_TRUE(branchB.Await().IsFulfilled());
    while (nDone.load() < 2)
    {
        std::this_thread::yield();
    }
    ASSERT_EQ(spCtx->nValue, 1);  // 首层那一次（两条分支只写各自字段，不动 nValue）
    ASSERT_EQ(spCtx->nForkA, 1);  // 分支 A 执行一次
    ASSERT_EQ(spCtx->nForkB, 1);  // 分支 B 执行一次
    exec.Stop();
}

/// @brief 链完成后再追加层：投递到执行器异步执行（不阻塞调用方）。
TEST(Promise_ThenAfterSettled)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    common::async::CPromise<CTestContext> chain = exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC);
    ASSERT_TRUE(chain.Await().IsFulfilled());  // 首层已完成

    std::atomic<bool> bDone(false);
    common::async::CPromise<CTestContext> tail = chain.Then(&StepAdd10, common::async::TaskKind::kWrite, ASYNC_LOC);
    tail.OnSettled(
        [&bDone](common::async::CPromiseResult)
        {
            bDone.store(true);
        });

    ASSERT_TRUE(tail.Await().IsFulfilled());
    while (!bDone.load())
    {
        std::this_thread::yield();
    }
    ASSERT_EQ(spCtx->nValue, 11);
    exec.Stop();
}

// ==================== 执行器生命周期与线程模型 ====================

/// @brief 未启动执行器起 promise → 立即以框架侧拒绝「执行器已停」收口，不阻塞。
TEST(Promise_NotStarted)
{
    common::async::CAsyncExecutor exec(2);
    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();

    common::async::CPromise<CTestContext> tail = exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                     .Then(&StepAdd10, common::async::TaskKind::kWrite, ASYNC_LOC);
    const common::async::CPromiseResult r = tail.Await();

    ASSERT_TRUE(r.IsRejected());
    ASSERT_TRUE(asynctest::IsStoppedFailure(r));  // 框架侧拒绝「执行器已停」
    ASSERT_EQ(spCtx->nSteps, 0);
}

/// @brief Stop 之后起 promise → 以框架侧拒绝「执行器已停」收口。
TEST(Promise_AfterStop)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());
    exec.Stop();

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    const common::async::CPromiseResult r = exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC).Await();
    ASSERT_TRUE(r.IsRejected());
    ASSERT_TRUE(asynctest::IsStoppedFailure(r));
}

/// @brief Stop 之后重新 Start：可继续起链。
TEST(Promise_RestartExecutor)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx1 = std::make_shared<CTestContext>();
    ASSERT_TRUE(exec.NewPromise(spCtx1, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC).Await().IsFulfilled());
    exec.Stop();

    ASSERT_TRUE(exec.Start());  // 重新启动（重建句柄与线程池）
    std::shared_ptr<CTestContext> spCtx2 = std::make_shared<CTestContext>();
    ASSERT_TRUE(exec.NewPromise(spCtx2, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC).Await().IsFulfilled());
    ASSERT_EQ(spCtx2->nValue, 1);
    exec.Stop();
}

/// @brief 层在工作线程上执行（不在起链线程）。
TEST(Promise_WorkerThread)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    const std::thread::id mainId = std::this_thread::get_id();
    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    ASSERT_TRUE(exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC).Await().IsFulfilled());
    ASSERT_TRUE(spCtx->workerId != mainId);
    exec.Stop();
}

/// @brief 执行器析构后，已起的链仍安全完成（句柄保活线程池）。
TEST(Promise_LifetimeAfterDestroy)
{
    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    // 句柄没有默认构造：要跨作用域持有，就用 shared_ptr 装它（链本身是浅句柄，可拷贝）。
    std::shared_ptr<common::async::CPromise<CTestContext> > spTail;
    {
        common::async::CAsyncExecutor exec(2);
        ASSERT_TRUE(exec.Start());
        spTail = std::make_shared<common::async::CPromise<CTestContext> >(
            exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC)
                .Then(&StepAdd10, common::async::TaskKind::kWrite, ASYNC_LOC));
        exec.Stop();  // 等待已投递任务完成
    }  // 执行器析构

    const common::async::CPromiseResult r = spTail->Await();
    ASSERT_TRUE(r.IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 11);
}

/// @brief 多线程等待同一链（并发 Get）。
TEST(Promise_ConcurrentAwait)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    common::async::CPromise<CTestContext> tail = exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                     .Then(&StepAdd10, common::async::TaskKind::kWrite, ASYNC_LOC);

    std::atomic<int> nOk(0);
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
    {
        threads.push_back(std::thread(
            [&tail, &nOk]()
            {
                if (tail.Await().IsFulfilled())
                {
                    nOk.fetch_add(1);
                }
            }));
    }
    for (size_t i = 0; i < threads.size(); ++i)
    {
        threads[i].join();
    }
    ASSERT_EQ(nOk.load(), 8);
    ASSERT_EQ(spCtx->nSteps, 2);  // 层不会被重复执行
    exec.Stop();
}

/// @brief 多条链并行执行（不被串行化）。显式声明为「读」链 —— 读任务之间可并发；
///        默认的写链会与模块内其它任务互斥（见 Async/ReadWriteGate.h）。
TEST(Promise_ParallelPromises)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    std::atomic<int> nActive(0);
    std::atomic<int> nPeak(0);
    const int kChains = 8;

    std::vector<common::async::CPromise<CTestContext> > chains;
    for (int i = 0; i < kChains; ++i)
    {
        std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
        common::async::CPromise<CTestContext> chain = exec.NewPromise(
            spCtx,
            [&nActive, &nPeak](const std::shared_ptr<CTestContext>& /*spCtx*/)
            {
                const int nNow = nActive.fetch_add(1) + 1;
                int nCur = nPeak.load();
                while (nCur < nNow && !nPeak.compare_exchange_weak(nCur, nNow))
                {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                nActive.fetch_sub(1);
                return common::async::CPromiseResult::Resolve();
            },
            common::async::TaskKind::kRead, ASYNC_LOC);
        chains.push_back(chain);
    }

    for (size_t i = 0; i < chains.size(); ++i)
    {
        ASSERT_TRUE(chains[i].Await().IsFulfilled());
    }
    ASSERT_TRUE(nPeak.load() >= 2);  // 4 线程下多条链应并行（而非串行）
    exec.Stop();
}

/// @brief 深链：层数超过内联深度上限仍正确执行（超限改投递，防爆栈）。
TEST(Promise_DeepChain)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    const int kLayers = 300;
    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    common::async::CPromise<CTestContext> tail = exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC);
    for (int i = 1; i < kLayers; ++i)
    {
        tail = tail.Then(&StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC);
    }
    const common::async::CPromiseResult r = tail.Await();

    ASSERT_TRUE(r.IsFulfilled());
    ASSERT_EQ(spCtx->nSteps, kLayers);
    ASSERT_EQ(spCtx->nValue, kLayers);
    exec.Stop();
}

/// @brief 压力：大量链 × 多层，全部完成且计数精确。
TEST(Promise_Stress)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    const int kChains = 400;
    std::atomic<long> nSteps(0);
    std::vector<common::async::CPromise<CTestContext> > tails;
    tails.reserve(kChains);
    for (int i = 0; i < kChains; ++i)
    {
        std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
        auto step = [&nSteps](const std::shared_ptr<CTestContext>& sp)
        {
            ++sp->nSteps;
            nSteps.fetch_add(1);
            return common::async::CPromiseResult::Resolve();
        };
        tails.push_back(exec.NewPromise(spCtx, step, common::async::TaskKind::kWrite, ASYNC_LOC)
                            .Then(step, common::async::TaskKind::kWrite, ASYNC_LOC)
                            .Then(step, common::async::TaskKind::kWrite, ASYNC_LOC));
    }

    int nOk = 0;
    for (size_t i = 0; i < tails.size(); ++i)
    {
        if (tails[i].Await().IsFulfilled())
        {
            ++nOk;
        }
    }
    ASSERT_EQ(nOk, kChains);
    ASSERT_EQ(nSteps.load(), static_cast<long>(kChains) * 3);
    exec.Stop();
}

/// @brief Post：未启动 / 已停止时不接受；启动后任务在工作线程执行。
TEST(Promise_PostBehavior)
{
    common::async::CAsyncExecutor exec(2);
    std::atomic<int> nDone(0);
    ASSERT_TRUE(!exec.Post(common::async::TaskKind::kWrite,
        [&nDone]()
        {
            nDone.fetch_add(1);
        }));  // 未启动

    ASSERT_TRUE(exec.Start());
    const std::thread::id mainId = std::this_thread::get_id();
    std::thread::id workerId;
    ASSERT_TRUE(exec.Post(common::async::TaskKind::kWrite,
        [&nDone, &workerId, mainId]()
        {
            workerId = std::this_thread::get_id();
            (void)mainId;
            nDone.fetch_add(1);
        }));
    exec.Stop();  // 等待任务完成
    ASSERT_EQ(nDone.load(), 1);
    ASSERT_TRUE(workerId != mainId);

    ASSERT_TRUE(!exec.Post(common::async::TaskKind::kWrite,
        [&nDone]()
        {
            nDone.fetch_add(1);
        }));  // 已停止
    ASSERT_EQ(nDone.load(), 1);
}

// ==================== 协程 ====================

/// 协程：顺序 await 两条子链（协程与子链共享同一上下文）。
class CSequentialCoro : public common::async::CCoroutine<CTestContext>
{
public:
    using common::async::CCoroutine<CTestContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(common::async::TaskKind::kWrite, NewPromise(&StepAdd1, common::async::TaskKind::kWrite));
        CO_AWAIT(common::async::TaskKind::kWrite, NewPromise(&StepAdd10, common::async::TaskKind::kWrite));
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 协程：并行 await 两条子链。
class CParallelCoro : public common::async::CCoroutine<CTestContext>
{
public:
    using common::async::CCoroutine<CTestContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_ALL(common::async::TaskKind::kWrite, NewPromise(&StepParA, common::async::TaskKind::kWrite),
            NewPromise(&StepParB, common::async::TaskKind::kWrite));
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 协程：await 到失败 → 终止（失败码透传，后续 await 不执行）。
class CFailCoro : public common::async::CCoroutine<CTestContext>
{
public:
    using common::async::CCoroutine<CTestContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(common::async::TaskKind::kWrite, NewPromise(&StepFail, common::async::TaskKind::kWrite));
        CO_AWAIT(common::async::TaskKind::kWrite, NewPromise(&StepShouldNotRun, common::async::TaskKind::kWrite));  // 不执行
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 协程：显式以失败结果结束（CO_RETURN）。
class CReturnFailCoro : public common::async::CCoroutine<CTestContext>
{
public:
    using common::async::CCoroutine<CTestContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(common::async::TaskKind::kWrite, NewPromise(&StepAdd1, common::async::TaskKind::kWrite));
        CO_RETURN(common::async::CPromiseResult::Reject(std::runtime_error("协程显式拒绝")));
        CO_END();
    }
};

/// 协程：协程体里直接抛异常（未捕获）—— 框架应把它收口为「本协程拒绝」。
class CThrowCoro : public common::async::CCoroutine<CTestContext>
{
public:
    using common::async::CCoroutine<CTestContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        throw std::runtime_error("协程体抛异常");
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 子协程：await 一条子链。
class CChildCoro : public common::async::CCoroutine<CTestContext>
{
public:
    using common::async::CCoroutine<CTestContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(common::async::TaskKind::kWrite, NewPromise(&StepAdd10, common::async::TaskKind::kWrite));
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 父协程：await 子协程（嵌套）。
class CParentCoro : public common::async::CCoroutine<CTestContext>
{
public:
    explicit CParentCoro(const std::shared_ptr<CTestContext>& spCtx, common::async::CAsyncExecutor* pExec)
        : common::async::CCoroutine<CTestContext>(spCtx), m_pExec(pExec), m_pChild()
    {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(common::async::TaskKind::kWrite, NewPromise(&StepAdd1, common::async::TaskKind::kWrite));
        m_pChild = m_pExec->CoStart<CChildCoro>(common::async::TaskKind::kWrite, GetContext());  // 跨 await 的变量须为成员
        CO_AWAIT(common::async::TaskKind::kWrite, m_pChild->AsPromise());
        CO_RETURN_VOID();
        CO_END();
    }

private:
    common::async::CAsyncExecutor* m_pExec;
    std::shared_ptr<CChildCoro> m_pChild;
};

/// @brief 协程顺序 await：子链共用协程上下文，最终结果为成功。
TEST(Coro_Sequential)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CSequentialCoro> pCoro = exec.CoStart<CSequentialCoro>(common::async::TaskKind::kWrite, spCtx);

    ASSERT_TRUE(pCoro->Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 11);                   // 1 + 10
    ASSERT_EQ(spCtx->strTrace, std::string("12"));  // 顺序确定
    ASSERT_TRUE(pCoro->GetContext() == spCtx);      // 协程与子链共享同一上下文
    exec.Stop();
}

/// @brief 协程并行 await（CO_AWAIT_ALL）。
///
/// 两条子链在同一上下文上并发跑，所以「各写自己的字段」（框架只保证同一条链的层顺序执行，
/// 跨链并发由调用方负责 —— 都写 `nValue` 就是数据竞争，TSan 会报）。
TEST(Coro_Parallel)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CParallelCoro> pCoro = exec.CoStart<CParallelCoro>(common::async::TaskKind::kWrite, spCtx);

    ASSERT_TRUE(pCoro->Await().IsFulfilled());
    ASSERT_EQ(spCtx->nParA.load(), 1);  // 分支 A 执行一次
    ASSERT_EQ(spCtx->nParB.load(), 1);  // 分支 B 执行一次（并行，先后不定）
    exec.Stop();
}

/// @brief 协程 await 到失败 → 协程终止，失败码透传，后续 await 不执行。
TEST(Coro_AwaitRejected)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    spCtx->strFailText = "业务拒绝（协程内失败）";
    std::shared_ptr<CFailCoro> pCoro = exec.CoStart<CFailCoro>(common::async::TaskKind::kWrite, spCtx);

    const common::async::CPromiseResult r = pCoro->Await();
    ASSERT_TRUE(r.IsRejected());
    ASSERT_EQ(r.Message(), spCtx->strFailText);
    ASSERT_EQ(spCtx->strTrace, std::string("F"));  // 后续层未执行
    exec.Stop();
}

/// @brief 协程显式以失败结束（CO_RETURN(Failed(...))）。
TEST(Coro_ReturnRejected)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CReturnFailCoro> pCoro = exec.CoStart<CReturnFailCoro>(common::async::TaskKind::kWrite, spCtx);

    const common::async::CPromiseResult r = pCoro->Await();
    ASSERT_TRUE(r.IsRejected());
    ASSERT_EQ(r.Message(), std::string("协程显式拒绝"));
    ASSERT_EQ(spCtx->nValue, 1);  // await 的子链已执行
    exec.Stop();
}

/// @brief 协程体抛异常 → 本协程以该异常收口（拒绝）：`Await()` 不会永久挂住。
///
/// 为什么必须有这条：恢复路径的上游是线程池 worker（不捕获异常），而执行器的 guard 只会
/// 记一条诊断、**不会 settle 协程** —— 不兜住就是「协程永久 pending + Await() 死等」
/// （本用例带超时等落定，坏了只是失败，不会挂住）。
TEST(Coro_BodyThrowSettlesRejected)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CThrowCoro> pCoro = exec.CoStart<CThrowCoro>(common::async::TaskKind::kWrite, spCtx);

    // 落定通知先挂好（恒送达）：可能在本行之前就已落定 → 立即投递触发。
    std::atomic<bool> bSettled(false);
    pCoro->AsPromise().OnSettled(
        [&bSettled](common::async::CPromiseResult /*result*/)
        {
            bSettled.store(true);
        });

    ASSERT_TRUE(asynctest::WaitFlag(bSettled, 1500));  // 带超时：实现坏了不会挂住用例
    const common::async::CPromiseResult r = pCoro->Await();
    ASSERT_TRUE(r.IsRejected());
    ASSERT_TRUE(r.Message().find("协程体抛异常") != std::string::npos);  // 文案带走
    exec.Stop();
}

/// @brief 嵌套：协程 await 子协程（AsPromise），共用同一上下文。
TEST(Coro_Nested)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CParentCoro> pCoro = exec.CoStart<CParentCoro>(common::async::TaskKind::kWrite, spCtx, &exec);

    ASSERT_TRUE(pCoro->Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 11);                   // 父 1 + 子 10
    ASSERT_EQ(spCtx->strTrace, std::string("12"));  // 顺序确定
    exec.Stop();
}

/// @brief 未启动执行器起协程 → 立即以框架侧拒绝「执行器已停」收口，Await 不阻塞。
TEST(Coro_NotStarted)
{
    common::async::CAsyncExecutor exec(2);
    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CSequentialCoro> pCoro = exec.CoStart<CSequentialCoro>(common::async::TaskKind::kWrite, spCtx);

    const common::async::CPromiseResult r = pCoro->Await();
    ASSERT_TRUE(r.IsRejected());
    ASSERT_TRUE(asynctest::IsStoppedFailure(r));
}

/// @brief 协程可重复启动（每次 CoStart 都是一个新对象，复用同一上下文）。
TEST(Coro_Restart)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CSequentialCoro> pCoro = exec.CoStart<CSequentialCoro>(common::async::TaskKind::kWrite, spCtx);
    ASSERT_TRUE(pCoro->Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 11);

    // 再次启动：内部会先复位状态再投递首次执行（对外只有 CoStart 一条启动路径）。
    std::shared_ptr<CSequentialCoro> pCoroAgain = exec.CoStart<CSequentialCoro>(common::async::TaskKind::kWrite, spCtx);
    ASSERT_TRUE(pCoroAgain->Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 22);
    exec.Stop();
}

/// @brief 协程作为可等待 promise 注册 settled 通知（AsPromise + OnSettled）。
TEST(Coro_OnSettledCallback)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CSequentialCoro> pCoro = exec.CoStart<CSequentialCoro>(common::async::TaskKind::kWrite, spCtx);

    std::atomic<int> nSettled(0);
    pCoro->AsPromise().OnSettled(
        [&nSettled](common::async::CPromiseResult r)
        {
            nSettled.store(r.IsFulfilled() ? 1 : -1);
        });
    while (nSettled.load() == 0)
    {
        std::this_thread::yield();
    }
    ASSERT_EQ(nSettled.load(), 1);  // 兑现
    exec.Stop();
}

/// @brief then 与 catch 互补：then 在兑现时执行、被拒绝时跳过；catch 相反。
TEST(Promise_ThenAndCatchComplement)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    // 兑现：then 执行、catch 不执行。
    std::shared_ptr<CTestContext> spOk = std::make_shared<CTestContext>();
    common::async::CPromise<CTestContext> tailOk = exec.NewPromise(spOk, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                       .Then(&StepAdd10, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                       .Catch(&StepCatchObserve, common::async::TaskKind::kWrite, ASYNC_LOC);
    ASSERT_TRUE(tailOk.Await().IsFulfilled());
    ASSERT_EQ(spOk->nValue, 11);     // then 路径已执行
    ASSERT_EQ(spOk->nCatchRuns, 0);  // catch 未执行（上一层兑现）
    ASSERT_EQ(spOk->strTrace, std::string("12"));

    // 拒绝：catch 执行（upResult 携带拒绝结果）、后续 then 跳过。
    std::shared_ptr<CTestContext> spBad = std::make_shared<CTestContext>();
    spBad->strFailText = "业务拒绝（then/catch 互补）";
    common::async::CPromise<CTestContext> tailBad = exec.NewPromise(spBad, &StepFail, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                        .Then(&StepShouldNotRun, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                        .Catch(&StepCatchObserve, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                        .Then(&StepShouldNotRun, common::async::TaskKind::kWrite, ASYNC_LOC);
    const common::async::CPromiseResult r = tailBad.Await();
    ASSERT_TRUE(r.IsRejected());
    ASSERT_EQ(r.Message(), spBad->strFailText);
    ASSERT_EQ(spBad->nCatchRuns, 1);                // catch 执行了
    ASSERT_EQ(spBad->nSeenFailed, 1);               // 真的看到了上一层的拒绝
    ASSERT_EQ(spBad->strTrace, std::string("FA"));  // 拒绝层 + catch（无 then 层痕迹）
    exec.Stop();
}

/// @brief finally：无论兑现还是拒绝都执行，且不改结果（忽略处理器返回值）。
TEST(Promise_FinallyKeepsResult)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    // 拒绝路径：finally 执行，但结果仍为拒绝（不像 catch 那样能吞掉拒绝）。
    std::shared_ptr<CTestContext> spBad = std::make_shared<CTestContext>();
    spBad->strFailText = "业务拒绝（finally 不改结果）";
    const common::async::CPromiseResult rBad =
        exec.NewPromise(spBad, &StepFail, common::async::TaskKind::kWrite, ASYNC_LOC)
            .Finally(&StepCatchRecover, common::async::TaskKind::kWrite, ASYNC_LOC)  // 返回 Resolve() 但被忽略
            .Await();
    ASSERT_TRUE(rBad.IsRejected());
    ASSERT_EQ(rBad.Message(), spBad->strFailText);
    ASSERT_EQ(spBad->nCatchRuns, 1);  // finally 已执行

    // 兑现路径：finally 也执行，结果仍为兑现。
    std::shared_ptr<CTestContext> spOk = std::make_shared<CTestContext>();
    const common::async::CPromiseResult rOk = exec.NewPromise(spOk, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                  .Finally(&StepCatchRecover, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                  .Await();
    ASSERT_TRUE(rOk.IsFulfilled());
    ASSERT_EQ(spOk->nCatchRuns, 1);
    ASSERT_EQ(spOk->nValue, 1);
    exec.Stop();
}

/// @brief 起链即投递首层：exec.NewPromise(spCtx, 首层) 等价 JS new Promise(executor)。
TEST(Promise_NewPromiseStarts)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    common::async::CPromise<CTestContext> p =
        exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC);  // 起链即投递首层
    ASSERT_TRUE(p.Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 1);

    common::async::CPromise<CTestContext> p2 = p.Then(&StepAdd10, common::async::TaskKind::kWrite, ASYNC_LOC);
    ASSERT_TRUE(p2.Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 11);
    exec.Stop();
}

// ==================== 嵌套：跨上下文 await ====================

/// @brief 子流程上下文（与 CTestContext 「不同」 —— 验证跨上下文 await）。
struct COtherContext
{
    int nRows;  ///< 子流程查询到的行数。

    COtherContext() : nRows(0)
    {}
};

/// 处理器（子流程）：置 nRows = 3。
static common::async::CPromiseResult StepQueryRows(const std::shared_ptr<COtherContext>& spCtx)
{
    spCtx->nRows = 3;
    return common::async::CPromiseResult::Resolve();
}

/// 协程：await 另一套上下文的子流程（跨上下文嵌套）。
class CCrossContextCoro : public common::async::CCoroutine<CTestContext>
{
public:
    explicit CCrossContextCoro(const std::shared_ptr<CTestContext>& spCtx, common::async::CAsyncExecutor* pExec)
        : common::async::CCoroutine<CTestContext>(spCtx), m_pExec(pExec), m_spOther()
    {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(common::async::TaskKind::kWrite, NewPromise(&StepAdd1, common::async::TaskKind::kWrite));  // 同上下文子 promise
        m_spOther = std::make_shared<COtherContext>();  // 跨 await → 成员变量
        CO_AWAIT(common::async::TaskKind::kWrite,
            m_pExec->NewPromise(m_spOther, &StepQueryRows, common::async::TaskKind::kWrite, ASYNC_LOC));  // 跨上下文 await
        GetContext()->nValue += m_spOther->nRows;                                                         // 恢复后并入
        CO_RETURN_VOID();
        CO_END();
    }

private:
    common::async::CAsyncExecutor* m_pExec;
    std::shared_ptr<COtherContext> m_spOther;
};

/// @brief 跨上下文嵌套：协程 await 另一套 TContext 的子流程。
TEST(Coro_AwaitOtherContext)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CCrossContextCoro> pCoro = exec.CoStart<CCrossContextCoro>(common::async::TaskKind::kWrite, spCtx, &exec);

    ASSERT_TRUE(pCoro->Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 4);  // StepAdd1（1）+ 子流程 3 行
    ASSERT_EQ(spCtx->nSteps, 1);
    exec.Stop();
}

/// 协程：并行 await「同上下文 + 两套别的上下文」的混合列表。
class CMixedParallelCoro : public common::async::CCoroutine<CTestContext>
{
public:
    explicit CMixedParallelCoro(const std::shared_ptr<CTestContext>& spCtx, common::async::CAsyncExecutor* pExec)
        : common::async::CCoroutine<CTestContext>(spCtx), m_pExec(pExec), m_spOtherA(), m_spOtherB()
    {}

    void Run() override
    {
        CO_BEGIN();
        m_spOtherA = std::make_shared<COtherContext>();  // 跨 await → 成员变量
        m_spOtherB = std::make_shared<COtherContext>();
        CO_AWAIT_ALL(common::async::TaskKind::kWrite, NewPromise(&StepAdd1, common::async::TaskKind::kWrite),  // 同上下文
            m_pExec->NewPromise(m_spOtherA, &StepQueryRows, common::async::TaskKind::kWrite, ASYNC_LOC),  // 另一套上下文
            m_pExec->NewPromise(m_spOtherB, &StepQueryRows, common::async::TaskKind::kWrite, ASYNC_LOC));
        GetContext()->nValue += m_spOtherA->nRows + m_spOtherB->nRows;
        CO_RETURN_VOID();
        CO_END();
    }

private:
    common::async::CAsyncExecutor* m_pExec;
    std::shared_ptr<COtherContext> m_spOtherA;
    std::shared_ptr<COtherContext> m_spOtherB;
};

/// @brief 并行 await 混合上下文列表（CO_AWAIT_ALL 支持每条 promise 不同类型）。
TEST(Coro_ParallelAwaitMixedContext)
{
    common::async::CAsyncExecutor exec(3);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CMixedParallelCoro> pCoro = exec.CoStart<CMixedParallelCoro>(common::async::TaskKind::kWrite, spCtx, &exec);

    ASSERT_TRUE(pCoro->Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 7);  // 1 + 3 + 3
    exec.Stop();
}

// ==================== 纯异步组合：new Promise + then-promise（无协程、零阻塞） ====================

/// 别的模块的拒绝文案（拒绝统一用标准异常表达）。
static const char* const kOtherModuleFailedText = "别的模块（子流程）失败";

/// 处理器（子流程）：模拟别的模块失败。
static common::async::CPromiseResult StepQueryRowsFail(const std::shared_ptr<COtherContext>& spCtx)
{
    spCtx->nRows = -1;
    return common::async::CPromiseResult::Reject(std::runtime_error(kOtherModuleFailedText));
}

/// @brief 跨模块组合（成功路径）：new Promise 桥接别的 promise + then-promise 等它。
///
/// 本流程：本模块层 → 桥接（别的模块的 promise，另一套上下文）→ 汇总 → 本模块层；
/// 全程回调驱动、不占工作线程（exec 只有 2 个 worker 也能跑）。
TEST(Promise_BridgeForeignPromise)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    spCtx->nValue = 100;
    std::shared_ptr<COtherContext> spOther = std::make_shared<COtherContext>();

    // 别的模块（另一套 TContext）的异步 promise。
    std::shared_ptr<common::async::CPromise<COtherContext> > spForeign(new common::async::CPromise<COtherContext>(
        exec.NewPromise(spOther, &StepQueryRows, common::async::TaskKind::kWrite, ASYNC_LOC)));

    common::async::CPromise<CTestContext> pTail =
        exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC)  // 本模块层
            .ThenPromise(
                [&exec, spForeign](const std::shared_ptr<CTestContext>& spCtxSelf)
                {
                    // new Promise：由「别的模块」的完成回调兑现 / 拒绝本 promise（非阻塞桥接）。
                    return exec.NewPromise(
                        spCtxSelf,

                        [spForeign, spCtxSelf](const common::async::CPromise<CTestContext>::ResolveFn& fnResolve,
                            const common::async::CPromise<CTestContext>::RejectFn& fnReject)
                        {
                            spForeign->OnSettled(
                                [spForeign, spCtxSelf, fnResolve, fnReject](common::async::CPromiseResult result)
                                {
                                    if (result.IsRejected())
                                    {
                                        fnReject(result);  // 拒绝：整份结果转交（异常类型 + 文案）。
                                        return;
                                    }
                                    spCtxSelf->nValue += spForeign->GetContext()->nRows;  // 汇总别的模块的数据。
                                    fnResolve();
                                });
                        },
                        common::async::TaskKind::kWrite, ASYNC_LOC);
                },
                common::async::TaskKind::kWrite, ASYNC_LOC)
            .Then(&StepAdd10, common::async::TaskKind::kWrite, ASYNC_LOC);  // 子 promise 完成后继续本模块层

    ASSERT_TRUE(pTail.Await().IsFulfilled());
    ASSERT_EQ(spOther->nRows, 3);
    ASSERT_EQ(spCtx->nValue, 114);  // 100 + 1（本模块层）+ 3（子流程）+ 10（本模块层）
    ASSERT_EQ(spCtx->nSteps, 2);
    exec.Stop();
}

/// @brief 跨模块组合（拒绝路径）：子 promise 被拒绝 → 本流程 then 层不执行、Catch 仍可处理。
TEST(Promise_BridgeForeignRejected)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<COtherContext> spOther = std::make_shared<COtherContext>();

    std::shared_ptr<common::async::CPromise<COtherContext> > spForeign(new common::async::CPromise<COtherContext>(
        exec.NewPromise(spOther, &StepQueryRowsFail, common::async::TaskKind::kWrite, ASYNC_LOC)));

    common::async::CPromise<CTestContext> pTail =
        exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC)
            .ThenPromise(
                [&exec, spForeign](const std::shared_ptr<CTestContext>& spCtxSelf)
                {
                    return exec.NewPromise(
                        spCtxSelf,

                        [spForeign](const common::async::CPromise<CTestContext>::ResolveFn& fnResolve,
                            const common::async::CPromise<CTestContext>::RejectFn& fnReject)
                        {
                            spForeign->OnSettled(
                                [fnResolve, fnReject](common::async::CPromiseResult result)
                                {
                                    if (result.IsRejected())
                                    {
                                        fnReject(result);
                                        return;
                                    }
                                    fnResolve();
                                });
                        },
                        common::async::TaskKind::kWrite, ASYNC_LOC);
                },
                common::async::TaskKind::kWrite, ASYNC_LOC)
            .Then(&StepShouldNotRun, common::async::TaskKind::kWrite, ASYNC_LOC)    // 子 promise 被拒绝 → 不执行
            .Catch(&StepCatchObserve, common::async::TaskKind::kWrite, ASYNC_LOC);  // catch 仍执行（观察拒绝）

    const common::async::CPromiseResult result = pTail.Await();
    ASSERT_TRUE(result.IsRejected());
    ASSERT_EQ(result.Message(), std::string(kOtherModuleFailedText));  // 异常原样透传
    ASSERT_EQ(spCtx->nSteps, 1);                                       // 只有桥接前的 StepAdd1 执行
    ASSERT_EQ(spCtx->nSeenFailed, 1);                                  // catch 观察到拒绝
    ASSERT_EQ(spCtx->nCatchRuns, 1);
    exec.Stop();
}

/// 层（then）：用异常拒绝（拒绝与「兑现」完全分开，只看 `IsRejected()`）。
static common::async::CPromiseResult StepRejectZeroCode(const std::shared_ptr<CTestContext>& spCtx)
{
    ++spCtx->nSteps;
    spCtx->strTrace += "Z";
    return common::async::CPromiseResult::Reject(std::runtime_error("参数非法"));
}

/// 层（then）：用带运行时数字的长文案拒绝（长度超过 SSO 上限）。
static common::async::CPromiseResult StepRejectDynamicText(const std::shared_ptr<CTestContext>& spCtx)
{
    ++spCtx->nSteps;
    spCtx->strTrace += "F";
    return common::async::CPromiseResult::Reject(
        std::runtime_error("库存不足：需 " + std::to_string(spCtx->nValue + 3) + " 件，只剩 1 件（订单 SO-20260919-000123）"));
}

/// 层（then）：抛异常（异常对象原样成为本层拒绝，`what()` 不丢）。
static common::async::CPromiseResult StepThrowRuntimeError(const std::shared_ptr<CTestContext>& spCtx)
{
    ++spCtx->nSteps;
    throw std::runtime_error("磁盘写失败: /data/order.bin");
}

/// @brief 拒绝只看 `IsRejected()`（结果里没有任何码），异常描述原样透传到 catch。
TEST(Promise_RejectionIsJustAnException)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());
    const std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();

    std::string strCaughtMessage;
    const common::async::CPromise<CTestContext>::ResultHandler fnCatch =
        [&strCaughtMessage](common::async::CPromiseResult upResult, const std::shared_ptr<CTestContext>& /*spCtx*/)
    {
        strCaughtMessage = upResult.Message();
        return upResult;  // 透传拒绝（不改结果）
    };

    const common::async::CPromiseResult result = exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                     .Then(&StepRejectZeroCode, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                     .Then(&StepShouldNotRun, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                     .Catch(fnCatch, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                     .Await();

    ASSERT_TRUE(result.IsRejected());
    ASSERT_TRUE(!result.IsFulfilled());
    ASSERT_EQ(result.Message(), std::string("参数非法"));
    ASSERT_EQ(strCaughtMessage, std::string("参数非法"));  // catch 侧读到同一份
    ASSERT_EQ(spCtx->nSteps, 2);                           // 拒绝即停：后面的 then 未执行
    exec.Stop();
}

/// @brief 拒绝文案：动态（带运行时数字）、任意长度都能沿链透传；框架侧拒绝同样带文案。
TEST(Promise_RefusalMessageTravels)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());
    const std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();

    std::string strCaughtMessage;
    const common::async::CPromise<CTestContext>::ResultHandler fnCatch =
        [&strCaughtMessage](common::async::CPromiseResult upResult, const std::shared_ptr<CTestContext>& /*spCtx*/)
    {
        strCaughtMessage = upResult.Message();
        return upResult;
    };

    // 业务拒绝：动态长文案（StepAdd1 把 nValue 变成 1 → 文案里是「需 4 件」）。
    const common::async::CPromiseResult rBiz = exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                   .Then(&StepRejectDynamicText, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                   .Catch(fnCatch, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                   .Await();
    ASSERT_TRUE(rBiz.IsRejected());
    ASSERT_EQ(rBiz.Message(), std::string("库存不足：需 4 件，只剩 1 件（订单 SO-20260919-000123）"));
    ASSERT_TRUE(rBiz.Message().size() > 15);      // 超出 SSO 上限也完整保留
    ASSERT_EQ(strCaughtMessage, rBiz.Message());  // catch 侧读到同一份

    // 框架侧①：处理器抛异常 → 异常对象原样成为拒绝。
    const common::async::CPromiseResult rThrown = exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                      .Then(&StepThrowRuntimeError, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                      .Await();
    ASSERT_TRUE(rThrown.IsRejected());
    ASSERT_EQ(rThrown.Message(), std::string("磁盘写失败: /data/order.bin"));

    // 框架侧②：停掉的执行器 → 「执行器已停」（固定文案）。
    exec.Stop();
    const common::async::CPromiseResult rStopped =
        exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC).Await();
    ASSERT_TRUE(rStopped.IsRejected());
    ASSERT_TRUE(asynctest::IsStoppedFailure(rStopped));
    ASSERT_EQ(rStopped.Message(), std::string("执行器已停"));
}
