#include "ChainCase.h"

#include "Async/AsyncExecutor.h"
#include "framework/Bench.h"

#include <string>

void RunChainCases()
{
    const std::string group = "2. 任务链（CAsyncExecutor::Then 开销）";
    common::async::CAsyncExecutor exec(1);
    exec.Start();

    const int lens[] = { 1, 5, 20, 100 };

    // 直接函数链基线（长度 1 / 5 / 20 / 100）。
    for (size_t i = 0; i < 4; ++i)
    {
        const int n = lens[i];
        benchmark::BenchOp(group,
            "direct chain x" + std::to_string(n) + " (baseline)",
            [n]() {
                volatile int s;
                int v = 0;
                for (int k = 0; k < n; ++k)
                    v = v + 1;
                s = v;
                (void)s;
            },
            41, "循环内联，理论下限");
    }

    // 正确性校验（①）：Then 链最终值应为 10（防优化破坏功能）。
    {
        auto t = exec.Submit([]() { return 0; });
        for (int k = 0; k < 10; ++k)
            t = t.Then([](int x) { return x + 1; });
        benchmark::SanityCheck(group, "Then 链结果=10", t.Get().Value() == 10);
    }

    // CAsyncExecutor Then 链（长度 1 / 5 / 20 / 100）。
    // 注：数值为「构建 + 执行 + 取值」合计（每次 Then 含下游状态分配与续接登记），
    //     无法与执行完全分离（链构建后续接自动触发执行）。
    for (size_t i = 0; i < 4; ++i)
    {
        const int n = lens[i];
        benchmark::BenchOp(group, "CAsyncExecutor chain x" + std::to_string(n),
            [&exec, n]() {
                auto task = exec.Submit([]() { return 0; });
                for (int k = 0; k < n; ++k)
                    task = task.Then([](int x) { return x + 1; });
                volatile int s = task.Get().Value();
                (void)s;
            },
            21, "构建 n 级 Then + 执行 + 取值（合计）");
    }

    // 正确性校验（①）：flatMap 链最终值应为 10。
    {
        auto t = exec.Submit([]() { return 0; });
        for (int k = 0; k < 10; ++k)
            t = t.Then([&exec](int x) { return exec.Submit([x]() { return x + 1; }); });
        benchmark::SanityCheck(group, "flatMap 链结果=10", t.Get().Value() == 10);
    }

    // CAsyncExecutor flatMap 链（变换返回 CTask → 触发 FlatMapForward 转发路径）。
    for (size_t i = 0; i < 4; ++i)
    {
        const int n = lens[i];
        benchmark::BenchOp(group, "CAsyncExecutor flatMap chain x" + std::to_string(n),
            [&exec, n]() {
                auto task = exec.Submit([]() { return 0; });
                for (int k = 0; k < n; ++k)
                    task = task.Then([&exec](int x) {
                        return exec.Submit([x]() { return x + 1; }); // 返回 CTask → flatMap
                    });
                volatile int s = task.Get().Value();
                (void)s;
            },
            21, "n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径");
    }

    exec.Stop();
}
