// ============================================================
// AsyncExecutorNoThrow 完整示例（无异常版异步框架）
// 错误码 + CTaskResult，风格类似 std::expected<T, Error>
//
// 编译：
//   g++ -std=c++11 -pthread -I Common -I Common/ThirdParty/asio/asio/include
//       -DASIO_STANDALONE examples/nothrow_demo.cpp
//       Common/Async/AsyncExecutorNoThrow.cpp Common/Thread/ThreadPool.cpp
// ============================================================
#include "Async/AsyncExecutorNoThrow.h"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace no = common::nothrow;

// ① 简单任务：Submit + Get
void DemoSubmitAndGet()
{
    no::CAsyncExecutor<> exec(2);
    exec.Start();

    no::CTaskResult<int> r = exec.Submit([]() { return 42; }).Get();
    if (r.Ok())
    {
        std::printf("① Submit+Get: %d\n", r.Value());
    }
    exec.Stop();
}

// ② 链式调用：Submit → Then → Then → Get（同步变换）
void DemoChain()
{
    no::CAsyncExecutor<> exec(2);
    exec.Start();

    no::CTaskResult<int> r =
        exec.Submit([]() { return 3; })
            .Then([](int n) { return n * 2; })
            .Then([](int n) { return n + 1; })
            .Get();
    if (r.Ok())
    {
        std::printf("② 链式: (3*2)+1 = %d\n", r.Value());
    }
    exec.Stop();
}

// ③ flatMap：变换函数返回 CTask，自动平铺为下游结果
void DemoFlatMap()
{
    no::CAsyncExecutor<> exec(2);
    exec.Start();

    no::CTaskResult<std::string> r =
        exec.Submit([]() { return 3; })
            .Then([&exec](int n) {                // 返回新异步任务 → 自动平铺
                return exec.Submit([n]() { return n * n; });
            })
            .Then([](int n) { return "平方 = " + std::to_string(n); })
            .Get();
    if (r.Ok())
    {
        std::printf("③ flatMap: %s\n", r.Value().c_str());
    }
    exec.Stop();
}

// ④ 错误处理：任务异常转 kTaskFailed；业务错误码沿链传播
void DemoErrors()
{
    no::CAsyncExecutor<> exec(2);
    exec.Start();

    // 4.1 任务函数抛异常 → 框架捕获转 kTaskFailed
    no::CTaskResult<int> failed =
        exec.Submit([]() -> int { throw std::runtime_error("数据库连接失败"); }).Get();
    if (failed.Failed())
    {
        std::printf("④ 异常转码: code=%d msg=%s\n",
                    failed.Error().nCode, failed.Error().strMessage.c_str());
    }

    // 4.2 业务错误码（1001）沿链传播，后续 Then 不执行
    no::CTaskResult<int> bizErr =
        no::CTask<int>::FromResult(no::CTaskResult<int>::Failure(1001, "业务校验失败"))
            .Then([](int) { return 0; }) // 上游失败，此步不执行
            .Get();
    if (bizErr.Failed())
    {
        std::printf("    业务错误沿链: code=%d msg=%s\n",
                    bizErr.Error().nCode, bizErr.Error().strMessage.c_str());
    }
    exec.Stop();
}

// ⑤ void 任务 + OnSuccess / OnFailure 回调
void DemoVoidAndCallbacks()
{
    no::CAsyncExecutor<> exec(1);
    exec.Start();

    no::CTaskResult<void> vr = exec.Submit([]() { std::printf("⑤ void 任务执行\n"); }).Get();
    if (vr.Ok())
    {
        std::printf("   void 任务成功\n");
    }

    no::CTask<int> t = exec.Submit([]() { return 7; });
    t.OnSuccess([](const int& v) { std::printf("   OnSuccess: %d\n", v); });
    t.OnFailure([](const no::CTaskError& e) { std::printf("   OnFailure: %s\n", e.strMessage.c_str()); });
    t.Get();
    exec.Stop();
}

int main()
{
    DemoSubmitAndGet();
    DemoChain();
    DemoFlatMap();
    DemoErrors();
    DemoVoidAndCallbacks();
    return 0;
}
