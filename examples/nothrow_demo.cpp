// ============================================================
// AsyncExecutorNoThrow 完整示例（无异常版异步框架，Option 风格）
// 有值/无值，风格类似 Rust Option / C++ std::optional
//
// 编译：
//   g++ -std=c++11 -pthread -I Common -I Common/ThirdParty/asio/asio/include
//       -DASIO_STANDALONE examples/nothrow_demo.cpp
//       Common/Async/AsyncExecutorNoThrow.cpp Common/Thread/ThreadPool.cpp
// ============================================================
#include "Async/AsyncExecutorNoThrow.h"

#include <cstdio>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace no = common::nothrow;

// ============================================================
// 轻量 ASSERT（示例自我校验用，MFC 命名风格）
// 条件为假时打印位置并累计失败数，不中断程序。
// ============================================================
static int g_nAssertFailures = 0;

#define ASSERT(cond)                                                        \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            std::printf("  [ASSERT] 失败: %s (%s:%d)\n", #cond, __FILE__,  \
                        __LINE__);                                          \
            ++g_nAssertFailures;                                            \
        }                                                                   \
    } while (0)

// ① 简单任务：Submit + Get（有值）
void DemoSubmitAndGet()
{
    no::CAsyncExecutor exec(2);
    exec.Start();

    no::CTaskResult<int> r = exec.Submit([]() { return 42; }).Get();
    ASSERT(r.HasValue());          // 有值
    ASSERT(r.Value() == 42);       // 结果正确
    std::printf("① Submit+Get: %d\n", r.Value());
    exec.Stop();
}

// ② 链式调用：Submit → Then → Then → Get（有值传播）
void DemoChain()
{
    no::CAsyncExecutor exec(2);
    exec.Start();

    no::CTaskResult<int> r =
        exec.Submit([]() { return 3; })
            .Then([](int n) { return n * 2; })
            .Then([](int n) { return n + 1; })
            .Get();
    ASSERT(r.HasValue());          // 链式任务成功
    ASSERT(r.Value() == 7);        // (3*2)+1
    std::printf("② 链式: (3*2)+1 = %d\n", r.Value());
    exec.Stop();
}

// ③ flatMap：变换函数返回 CTask，自动平铺为下游结果
void DemoFlatMap()
{
    no::CAsyncExecutor exec(2);
    exec.Start();

    no::CTaskResult<std::string> r =
        exec.Submit([]() { return 3; })
            .Then([&exec](int n) {                // 返回新异步任务 → 自动平铺
                return exec.Submit([n]() { return n * n; });
            })
            .Then([](int n) { return "平方 = " + std::to_string(n); })
            .Get();
    ASSERT(r.HasValue());              // flatMap 任务成功
    ASSERT(r.Value() == "平方 = 9");   // 3*3 后转字符串
    std::printf("③ flatMap: %s\n", r.Value().c_str());
    exec.Stop();
}

// ④ 无值终止：任务异常转无值（kException）；None 终止沿链传播
void DemoNone()
{
    no::CAsyncExecutor exec(2);
    exec.Start();

    // 4.1 任务函数抛异常 → 框架捕获转无值终止（Reason=kException）
    no::CTaskResult<int> failed =
        exec.Submit([]() -> int { throw std::runtime_error("数据库连接失败"); }).Get();
    ASSERT(!failed.HasValue());                            // 异常被转成无值终止
    ASSERT(failed.Reason() == no::detail::kException);     // 终止原因：异常
    std::printf("④ 异常转无值: reason=%d（kException）\n",
                static_cast<int>(failed.Reason()));

    // 4.2 无值（None）终止沿链传播，后续 Then 不执行
    no::CTaskResult<int> bizNone =
        no::CTask<int>::FromResult(no::CTaskResult<int>())  // 无值（None）
            .Then([](int) { return 0; })                    // 上游无值，此步不执行
            .Get();
    ASSERT(!bizNone.HasValue());                            // 终止传播
    ASSERT(bizNone.Reason() == no::detail::kEndNone);       // 业务返回 None 终止
    std::printf("    None 终止沿链: reason=%d（kEndNone）\n",
                static_cast<int>(bizNone.Reason()));
    exec.Stop();
}

// ⑤ void 任务 + OnSuccess / OnNone 回调
void DemoVoidAndCallbacks()
{
    no::CAsyncExecutor exec(1);
    exec.Start();

    no::CTaskResult<void> vr = exec.Submit([]() { std::printf("⑤ void 任务执行\n"); }).Get();
    ASSERT(vr.HasValue());           // void 完成（HasValue = 完成）
    std::printf("   void 任务完成\n");

    no::CTask<int> t = exec.Submit([]() { return 7; });
    t.OnSuccess([](const int& v) {
        ASSERT(v == 7);              // 回调拿到的值正确
        std::printf("   OnSuccess: %d\n", v);
    });
    t.OnNone([](no::detail::CTaskEndReason) { std::printf("   OnNone: 链终止\n"); });
    t.Get();
    exec.Stop();
}

// ⑥ 用 std::shared_ptr<T> 在链中传递大对象（拷指针不拷内容）
void DemoSharedPtrChain()
{
    no::CAsyncExecutor exec(2);
    exec.Start();

    // 100 万个 int 的大对象，全程只传指针，零内容拷贝
    no::CTaskResult<long long> r =
        exec.Submit([]() {
                return std::make_shared<std::vector<int> >(1000000, 42);
            })
            .Then([](const std::shared_ptr<std::vector<int> >& p) {
                return std::make_shared<long long>(
                    std::accumulate(p->begin(), p->end(), 0LL));
            })
            .Then([](const std::shared_ptr<long long>& p) { return *p * 2; })
            .Get();
    ASSERT(r.HasValue());
    ASSERT(r.Value() == 84000000LL); // 1000000 * 42 * 2
    std::printf("⑥ shared_ptr 链式传递: %lld（零内容拷贝）\n",
                static_cast<long long>(r.Value()));
    exec.Stop();
}

// ⑦ 中途终止：中间函数返回 no::None → 后续 Then 自动跳过（正常提前结束）
void DemoEarlyTerminate()
{
    no::CAsyncExecutor exec(2);
    exec.Start();

    // 7.1 校验不通过 → 返回 None 终止链，后面的 Then 不执行
    no::CTaskResult<int> r =
        exec.Submit([]() { return -5; })
            .Then([](int n) -> no::CTaskResult<int> {
                if (n < 0)
                {
                    return no::None; // 无值 → 正常终止
                }
                return n * 2;        // 有值 → 传播
            })
            .Then([](int n) {
                return n + 1;
            }) // 上游终止，此步被跳过
            .Get();
    ASSERT(!r.HasValue());                              // 链条被提前终止
    ASSERT(r.Reason() == no::detail::kEndNone);         // 终止原因：业务 None
    std::printf("⑦ 中途终止: reason=%d（kEndNone，后续 Then 已跳过）\n",
                static_cast<int>(r.Reason()));

    // 7.2 对比：校验通过时链条正常接龙
    no::CTaskResult<int> r2 =
        exec.Submit([]() { return 5; })
            .Then([](int n) -> no::CTaskResult<int> {
                if (n < 0)
                {
                    return no::None;
                }
                return n * 2;
            })
            .Then([](int n) {
                return n + 1;
            }) // 正常执行
            .Get();
    ASSERT(r2.HasValue());
    ASSERT(r2.Value() == 11); // 5*2+1
    std::printf("    校验通过对比: %d\n", r2.Value());
    exec.Stop();
}

// ⑧ 值类型沿链变化：int → std::string → std::size_t
void DemoTypeChangingChain()
{
    no::CAsyncExecutor exec(2);
    exec.Start();

    no::CTaskResult<std::size_t> r =
        exec.Submit([]() { return 3; })                              // CTask<int>
            .Then([](int n) { return std::to_string(n * 10); }) // → CTask<std::string> "30"
            .Then([](const std::string& s) {                       // → CTask<std::size_t> 2
                std::printf("⑧ 类型变化链: int→string→size_t，长度=%zu\n", s.size());
                return s.size();
            })
            .Get();
    ASSERT(r.HasValue());
    ASSERT(r.Value() == static_cast<std::size_t>(2)); // "30" 的长度
    exec.Stop();
}

// ⑨ void 任务支持 Then：无参数往下传，可继续链
void DemoVoidThenChain()
{
    no::CAsyncExecutor exec(1);
    exec.Start();

    no::CTaskResult<int> r =
        exec.Submit([]() { std::printf("⑨ void 任务执行\n"); })  // CTask<void>
            .Then([]() { return 42; })                             // void → int（无参数传入）
            .Then([](int n) { return n + 8; })                     // int → int
            .Get();
    ASSERT(r.HasValue());
    ASSERT(r.Value() == 50); // 42+8
    std::printf("   void→Then 链式: %d\n", r.Value());
    exec.Stop();
}

int main()
{
    DemoSubmitAndGet();
    DemoChain();
    DemoFlatMap();
    DemoNone();
    DemoVoidAndCallbacks();
    DemoSharedPtrChain();
    DemoEarlyTerminate();
    DemoTypeChangingChain();
    DemoVoidThenChain();

    if (g_nAssertFailures == 0)
    {
        std::printf("全部断言通过 ✔\n");
        return 0;
    }
    std::printf("共 %d 个断言失败\n", g_nAssertFailures);
    return 1;
}
