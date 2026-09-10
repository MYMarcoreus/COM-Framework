#include "ChainCase.h"

#include <memory>
#include <string>

#include "Async/AsyncChain.h"
#include "Async/AsyncExecutor.h"
#include "cases/ChainContext.h"
#include "framework/Bench.h"

namespace {

/// 异步链引擎：在指定执行器上跑一条 n 层链，返回上下文累加值。
inline long long RunChain(common::async::CAsyncExecutor& exec, int nLayers)
{
    std::shared_ptr<bench::CChainContext> spCtx = std::make_shared<bench::CChainContext>();
    common::async::CAsyncChain<bench::CChainContext> tail = exec.Submit(spCtx, &bench::StepInc);
    for (int k = 1; k < nLayers; ++k)
    {
        tail = tail.Then(&bench::StepInc);
    }
    return tail.Get().IsOk() ? spCtx->nValue : -1;
}

}  // namespace

void RunChainCases()
{
    const std::string group = "2. 异步链（CAsyncChain 层开销）";
    common::async::CAsyncExecutor exec(1);
    exec.Start();

    const int lens[] = {1, 5, 20, 100};

    // 基线：等长直接函数链（理论下限）。
    for (size_t i = 0; i < 4; ++i)
    {
        const int n = lens[i];
        benchmark::BenchOp(group, "direct chain x" + std::to_string(n) + " (baseline)", [n]()
        {
            volatile long long s;
            long long v = 0;
            for (int k = 0; k < n; ++k)
            {
                v = v + 1;
            }
            s = v;
            (void)s;
        }, 41, "循环内联，理论下限");
    }

    // 正确性校验：链结果与失败即停语义。
    benchmark::SanityCheck(group, "链 10 层结果=10", RunChain(exec, 10) == 10);
    {
        std::shared_ptr<bench::CChainContext> spCtx = std::make_shared<bench::CChainContext>();
        common::async::CStepResult r = exec.Submit(spCtx, &bench::StepInc)
                                           .Then(&bench::StepFail)
                                           .Then(&bench::StepInc)  // 失败即停：不执行
                                           .Get();
        benchmark::SanityCheck(group, "失败即停（后续层不执行）", r.IsFailed() && spCtx->nSteps == 2);
    }

    // 异步链（1 / 5 / 20 / 100 层）：构建 + 执行 + 取值合计。
    for (size_t i = 0; i < 4; ++i)
    {
        const int n = lens[i];
        benchmark::BenchOp(group, "CAsyncChain x" + std::to_string(n), [&exec, n]()
        {
            volatile long long s = RunChain(exec, n);
            (void)s;
        }, 21, "构建 N 层链 + 首层投递 + 逐层级联 + Get");
    }

    // 深链：256 层（超过内联深度上限 kMaxInlineDepth=64 后，后续层改投递执行）。
    benchmark::BenchOp(group, "CAsyncChain deep x256 (inline→post)", [&exec]()
    {
        volatile long long s = RunChain(exec, 256);
        (void)s;
    }, 11, "深层链：内联 64 层后改投递，验证不爆栈");

    // 失败即停链：第 2 层失败 → 后续层全部短路（对比同长度成功链）。
    benchmark::BenchOp(group, "CAsyncChain fail-fast x20", [&exec]()
    {
        std::shared_ptr<bench::CChainContext> spCtx = std::make_shared<bench::CChainContext>();
        common::async::CAsyncChain<bench::CChainContext> tail =
            exec.Submit(spCtx, &bench::StepInc).Then(&bench::StepFail);
        for (int k = 0; k < 18; ++k)
        {
            tail = tail.Then(&bench::StepInc);  // 全部短路
        }
        volatile int s = tail.Get().Code();
        (void)s;
    }, 21, "失败后后续层短路（不执行层函数，仅透传结果）");

    exec.Stop();
}
