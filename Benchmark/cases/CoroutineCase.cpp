#include "CoroutineCase.h"

#include <memory>
#include <string>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"
#include "Coroutine/Coroutine.h"
#include "cases/ChainContext.h"
#include "framework/Bench.h"

namespace {

/// 协程：await 一条单层子链后完成。
class BenchCoroOnce : public common::async::CCoroutine<bench::CChainContext>
{
public:
    using common::async::CCoroutine<bench::CChainContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(&bench::StepInc));
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 协程：顺序 await 10 条单层子链（10 次挂起 / 恢复）。
class BenchCoroSeq10 : public common::async::CCoroutine<bench::CChainContext>
{
public:
    using common::async::CCoroutine<bench::CChainContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(&bench::StepInc));
        CO_AWAIT(NewPromise(&bench::StepInc));
        CO_AWAIT(NewPromise(&bench::StepInc));
        CO_AWAIT(NewPromise(&bench::StepInc));
        CO_AWAIT(NewPromise(&bench::StepInc));
        CO_AWAIT(NewPromise(&bench::StepInc));
        CO_AWAIT(NewPromise(&bench::StepInc));
        CO_AWAIT(NewPromise(&bench::StepInc));
        CO_AWAIT(NewPromise(&bench::StepInc));
        CO_AWAIT(NewPromise(&bench::StepInc));
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 协程：并行 await 10 条单层子链（CO_AWAIT_ALL）。
class BenchCoroAll10 : public common::async::CCoroutine<bench::CChainContext>
{
public:
    using common::async::CCoroutine<bench::CChainContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_ALL(NewPromise(&bench::StepInc), NewPromise(&bench::StepInc), NewPromise(&bench::StepInc),
            NewPromise(&bench::StepInc), NewPromise(&bench::StepInc), NewPromise(&bench::StepInc),
            NewPromise(&bench::StepInc), NewPromise(&bench::StepInc), NewPromise(&bench::StepInc),
            NewPromise(&bench::StepInc));
        CO_RETURN_VOID();
        CO_END();
    }
};

}  // namespace

void RunCoroutineCases()
{
    const std::string group = "3. 协程（CCoroutine 顺序化）";
    common::async::CAsyncExecutor exec(1);
    exec.Start();

    // 正确性校验：协程与子链共享上下文，结果一致。
    {
        std::shared_ptr<bench::CChainContext> spCtx = std::make_shared<bench::CChainContext>();
        std::shared_ptr<BenchCoroOnce> pCoro = exec.CoStart<BenchCoroOnce>(spCtx);
        benchmark::SanityCheck(group, "协程 1 次 await 结果=1", pCoro->Await().IsFulfilled() && spCtx->nValue == 1);
    }
    {
        std::shared_ptr<bench::CChainContext> spCtx = std::make_shared<bench::CChainContext>();
        std::shared_ptr<BenchCoroSeq10> pCoro = exec.CoStart<BenchCoroSeq10>(spCtx);
        benchmark::SanityCheck(group, "协程 10 次 await 结果=10", pCoro->Await().IsFulfilled() && spCtx->nValue == 10);
    }
    {
        std::shared_ptr<bench::CChainContext> spCtx = std::make_shared<bench::CChainContext>();
        std::shared_ptr<BenchCoroAll10> pCoro = exec.CoStart<BenchCoroAll10>(spCtx);
        benchmark::SanityCheck(
            group, "协程并行 await ×10 结果=10", pCoro->Await().IsFulfilled() && spCtx->nValue == 10);
    }

    // 基线：直接函数调用。
    benchmark::BenchOp(
        group, "direct_call (baseline)",
        []()
        {
            volatile int s = 42;
            (void)s;
        },
        7, "直接调用，无调度");

    // 单层 promise：NewPromise + Await（等价「一个异步步骤」）。
    benchmark::BenchOp(
        group, "CPromise single layer",
        [&exec]()
        {
            std::shared_ptr<bench::CChainContext> spCtx = std::make_shared<bench::CChainContext>();
            volatile long long s = exec.NewPromise(spCtx, &bench::StepInc).Await().IsFulfilled() ? spCtx->nValue : -1;
            (void)s;
        },
        7, "起 promise + 单层执行 + Await");

    // 协程：CoStart + 1 次 await + 完成。
    benchmark::BenchOp(
        group, "CCoroutine start+await",
        [&exec]()
        {
            std::shared_ptr<bench::CChainContext> spCtx = std::make_shared<bench::CChainContext>();
            std::shared_ptr<BenchCoroOnce> pCoro = exec.CoStart<BenchCoroOnce>(spCtx);
            volatile int s = pCoro->Await().Code();
            (void)s;
        },
        7, "CoStart → 1 次 CO_AWAIT（子链）→ 完成");

    // 10 层 promise：等效工作量（一次链构建）。
    benchmark::BenchOp(
        group, "CPromise x10",
        [&exec]()
        {
            std::shared_ptr<bench::CChainContext> spCtx = std::make_shared<bench::CChainContext>();
            common::async::CPromise<bench::CChainContext> tail = exec.NewPromise(spCtx, &bench::StepInc);
            for (int k = 1; k < 10; ++k)
            {
                tail = tail.Then(&bench::StepInc);
            }
            volatile long long s = tail.Await().IsFulfilled() ? spCtx->nValue : -1;
            (void)s;
        },
        7, "10 层 promise（构建 + 执行 + Await）");

    // 协程：10 次顺序 await（每条子链 1 层）。
    benchmark::BenchOp(
        group, "CCoroutine seq await x10",
        [&exec]()
        {
            std::shared_ptr<bench::CChainContext> spCtx = std::make_shared<bench::CChainContext>();
            std::shared_ptr<BenchCoroSeq10> pCoro = exec.CoStart<BenchCoroSeq10>(spCtx);
            volatile int s = pCoro->Await().Code();
            (void)s;
        },
        7, "10 次挂起 / 恢复（每次起一条单层子链）");

    // 协程：10 次并行 await。
    benchmark::BenchOp(
        group, "CCoroutine parallel await x10",
        [&exec]()
        {
            std::shared_ptr<bench::CChainContext> spCtx = std::make_shared<bench::CChainContext>();
            std::shared_ptr<BenchCoroAll10> pCoro = exec.CoStart<BenchCoroAll10>(spCtx);
            volatile int s = pCoro->Await().Code();
            (void)s;
        },
        7, "CO_AWAIT_ALL：10 条子链并行等待，一次恢复");

    exec.Stop();
}
