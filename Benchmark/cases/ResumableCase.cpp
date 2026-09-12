#include "ResumableCase.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "Async/AsyncExecutor.h"
#include "Async/Coroutine.h"
#include "Async/Promise.h"
#include "cases/ChainContext.h"
#include "framework/Bench.h"

namespace {

namespace no = common::async;

/// 协程：3 次顺序 await（批量伸缩测试的载荷）。
class BenchCoroSeq3 : public no::CCoroutine<bench::CChainContext>
{
public:
    using no::CCoroutine<bench::CChainContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(&bench::StepInc));
        CO_AWAIT(NewPromise(&bench::StepInc));
        CO_AWAIT(NewPromise(&bench::StepInc));
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 协程：20 次顺序 await（长协程：测每次挂起 / 恢复的摊销成本）。
class BenchCoroSeq20 : public no::CCoroutine<bench::CChainContext>
{
public:
    using no::CCoroutine<bench::CChainContext>::CCoroutine;

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

/// 批量起 nCoros 个协程并等待全部完成。
///
/// 协程对象经自持弱引用保活（框架内部 Resume 回调持强引用），
/// 这里不额外持有 shared_ptr（与真实业务「起完即走」的用法一致）。
inline int RunCoroBatch(no::CAsyncExecutor& exec, int nCoros)
{
    std::atomic<int> nDone(0);
    for (int i = 0; i < nCoros; ++i)
    {
        std::shared_ptr<bench::CChainContext> spCtx = std::make_shared<bench::CChainContext>();
        std::shared_ptr<BenchCoroSeq3> pCoro = exec.CoStart<BenchCoroSeq3>(spCtx);
        pCoro->AsPromise().OnSettled(
            [&nDone](no::CPromiseResult r)
            {
                if (r.IsFulfilled())
                {
                    nDone.fetch_add(1, std::memory_order_release);
                }
            });
    }
    while (nDone.load(std::memory_order_acquire) < nCoros)
    {
        std::this_thread::yield();
    }
    return nDone.load();
}

}  // namespace

void RunResumableCases()
{
    const std::string group = "4. 协程伸缩（长协程 / 批量并发）";

    // 长协程：单协程 20 次挂起 / 恢复全程成本（1 线程）。
    {
        no::CAsyncExecutor exec(1);
        exec.Start();

        {
            std::shared_ptr<bench::CChainContext> spCtx = std::make_shared<bench::CChainContext>();
            std::shared_ptr<BenchCoroSeq20> pCoro = exec.CoStart<BenchCoroSeq20>(spCtx);
            benchmark::SanityCheck(
                group, "长协程 20 次 await 结果=20", pCoro->Await().IsFulfilled() && spCtx->nValue == 20);
        }

        benchmark::BenchOp(
            group, "CCoroutine 20 awaits (1 thread)",
            [&exec]()
            {
                std::shared_ptr<bench::CChainContext> spCtx = std::make_shared<bench::CChainContext>();
                std::shared_ptr<BenchCoroSeq20> pCoro = exec.CoStart<BenchCoroSeq20>(spCtx);
                volatile int s = pCoro->Await().Code();
                (void)s;
            },
            11, "单协程 20 次挂起 / 恢复（每次 await 一条单层子链）");

        exec.Stop();
    }

    // 批量协程并发：同一载荷（200 个协程 × 3 次 await）在 1 / 2 / 4 线程下的总成本。
    const int kBatch = 200;
    const int nThreads[] = {1, 2, 4};
    for (size_t i = 0; i < 3; ++i)
    {
        no::CAsyncExecutor exec(static_cast<size_t>(nThreads[i]));
        exec.Start();

        benchmark::SanityCheck(group,
            "批量 " + std::to_string(kBatch) + " 协程全部完成 @" + std::to_string(nThreads[i]) + " thread",
            RunCoroBatch(exec, kBatch) == kBatch);

        benchmark::BenchOp(
            group, "batch " + std::to_string(kBatch) + " coro x3 await @" + std::to_string(nThreads[i]) + " thread",
            [&exec]()
            {
                volatile int s = RunCoroBatch(exec, kBatch);
                (void)s;
            },
            7, "一次逻辑操作 = " + std::to_string(kBatch) + " 个协程（各 3 次 await）全部完成");

        exec.Stop();
    }
}
