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

    // CAsyncExecutor Then 链（长度 1 / 5 / 20 / 100）。
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
            21, "构建 n 级 Then + 执行 + 取值");
    }

    exec.Stop();
}
