// ============================================================
// AsyncExecutor 完整示例（无异常版异步框架，Option 风格）
// 有值/无值，风格类似 Rust Option / C++ std::optional
//
// 项目结构参考标准服务器项目：main.cpp（入口）+ Linux/Makefile（构建配置）。
// 构建与运行：
//   cd examples/Linux && make run            # 构建并运行（release）
//   ./build.sh examples                      # 或经统一构建脚本（examples 为普通项目）
//
// 用法速览（每个函数演示一类用法）：
//   ① Submit+Get            最基本：提交任务、阻塞取结果
//   ② 链式 Then             有值逐级传播（Then → Then → Get）
//   ③ 变换返回 CTaskResult  有值传播 / return no::None 中途终止
//   ④ flatMap               变换返回 CTask，自动平铺
//   ⑤ None 沿链传播         上游终止 → 下游全部跳过
//   ⑥ 异常转无值            任务抛异常 → 无值（kException）
//   ⑦ void→void             无值任务之间继续接链
//   ⑧ void→值               void 任务 Then 返回普通值
//   ⑨ OnSuccess/OnNone      注册回调（fire-and-forget）
//   ⑪ shared_ptr 传大对象   链中拷指针不拷内容
//   ⑫ 并发多任务            多线程 Get 同一批任务
//   ⑬ 生命周期加固          执行器析构后任务仍安全完成
//   ⑭ Post                  提交无返回值任务（fire-and-forget）
//   ⑮ 未启动执行器          Submit 立即以无值（kNotStarted）完成
//   ⑯ 类型变化链            Then 链逐级改变返回值类型（int→string→size_t）
//   ⑰ 任务源码位置          NOTHROW_LOC 保存注册点函数/文件/行号（调试）
// ============================================================
#include "Async/AsyncExecutor.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace no = common::async;

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

// ① 最基本：Submit 提交任务，Get 阻塞取结果（有值）
void DemoSubmitAndGet()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    // NOTHROW_LOC：调试构建（make debug）下保存注册点的
    // __PRETTY_FUNCTION__ / __FILE__ / __LINE__，调试界面可查看。
    no::CTaskResult<int> r = exec.Submit([]() { return 42; }, NOTHROW_LOC).Get();
    ASSERT(r.HasValue());      // 有值
    ASSERT(r.Value() == 42);   // 结果正确
    std::printf("① Submit+Get: %d\n", r.Value());
    exec.Stop();
}

// ② 链式 Then：上一个的有值作为下一个的输入，逐级传播
void DemoChainThen()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    no::CTaskResult<int> r =
        exec.Submit([]() { return 3; }, NOTHROW_LOC)   // 保存函数/文件/行号
            .Then([](int n) { return n * 2; }, NOTHROW_LOC)
            .Then([](int n) { return n + 1; }, NOTHROW_LOC)
            .Get();
    ASSERT(r.HasValue());
    ASSERT(r.Value() == 7);        // (3*2)+1
    std::printf("② 链式 Then: (3*2)+1 = %d\n", r.Value());
    exec.Stop();
}

// ③ 变换返回 CTaskResult：有值传播；return no::None 中途终止
void DemoTransformResult()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    // 3.1 校验不通过 → 返回 None 终止链，后续 Then 跳过
    no::CTaskResult<int> r =
        exec.Submit([]() { return -5; })
            .Then([](int n) -> no::CTaskResult<int> {
                if (n < 0)
                {
                    return no::None; // 无值 → 正常终止
                }
                return n * 2;        // 有值 → 传播
            })
            .Then([](int n) { return n + 1; }) // 上游终止，此步被跳过
            .Get();
    ASSERT(!r.HasValue());                       // 链条被提前终止
    ASSERT(r.Reason() == no::detail::kEndNone);  // 终止原因：业务 None
    std::printf("③ 中途终止: reason=%d（kEndNone，后续 Then 已跳过）\n",
                static_cast<int>(r.Reason()));

    // 3.2 校验通过 → 链条正常接龙
    no::CTaskResult<int> r2 =
        exec.Submit([]() { return 5; })
            .Then([](int n) -> no::CTaskResult<int> {
                if (n < 0) return no::None;
                return n * 2;
            })
            .Then([](int n) { return n + 1; })
            .Get();
    ASSERT(r2.HasValue());
    ASSERT(r2.Value() == 11);        // 5*2+1
    std::printf("    校验通过对比: %d\n", r2.Value());
    exec.Stop();
}

// ④ flatMap：变换返回 CTask，内部任务完成后自动平铺
void DemoFlatMap()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    no::CTaskResult<std::string> r =
        exec.Submit([]() { return 3; }, NOTHROW_LOC)
            .Then([&exec](int n) {                 // 返回新异步任务 → 自动平铺
                return exec.Submit([n]() { return n * n; }, NOTHROW_LOC);
            })
            .Then([](int n) { return "平方 = " + std::to_string(n); }, NOTHROW_LOC)
            .Get();
    ASSERT(r.HasValue());
    ASSERT(r.Value() == "平方 = 9");   // 3*3 后转字符串
    std::printf("④ flatMap: %s\n", r.Value().c_str());
    exec.Stop();
}

// ⑤ None 沿链传播：上游任务无值 → 下游全部跳过，终止一路传到底
void DemoNonePropagation()
{
    no::CAsyncExecutor exec(1);
    ASSERT(exec.Start());

    no::CTaskResult<int> r =
        exec.Submit([]() -> int { throw std::runtime_error("boom"); }) // 上游无值
            .Then([](int n) { return n + 1; })                          // 此步被跳过
            .Get();
    ASSERT(!r.HasValue());
    ASSERT(r.Reason() == no::detail::kException);  // 终止原因透传
    std::printf("⑤ None 沿链传播: reason=%d（kException，后续 Then 已跳过）\n",
                static_cast<int>(r.Reason()));
    exec.Stop();
}

// ⑥ 异常转无值：任务函数抛异常 → 被框架捕获为无值（kException），不抛给调用方
void DemoExceptionToNone()
{
    no::CAsyncExecutor exec(1);
    ASSERT(exec.Start());

    no::CTaskResult<int> r =
        exec.Submit([]() -> int { throw std::runtime_error("数据库连接失败"); }).Get();
    ASSERT(!r.HasValue());                        // 异常被转成无值终止
    ASSERT(r.Reason() == no::detail::kException); // 终止原因：异常
    std::printf("⑥ 异常转无值: reason=%d（kException）\n",
                static_cast<int>(r.Reason()));
    exec.Stop();
}

// ⑦ void→void：void 任务之间继续接链（无参数传入，正常完成）
void DemoVoidToVoid()
{
    no::CAsyncExecutor exec(1);
    ASSERT(exec.Start());

    std::atomic<int> nSteps(0);
    no::CTaskResult<void> r =
        exec.Submit([&nSteps]() { nSteps.fetch_add(1); }) // void
            .Then([&nSteps]() { nSteps.fetch_add(1); })   // void → void
            .Get();
    ASSERT(r.HasValue());          // void→void 链正常完成
    ASSERT(nSteps.load() == 2);    // 两段都执行了
    std::printf("⑦ void→void: 完成，共 %d 步\n", nSteps.load());
    exec.Stop();
}

// ⑧ void → 值：void 任务 Then 返回普通值，继续正常链
void DemoVoidThenValue()
{
    no::CAsyncExecutor exec(1);
    ASSERT(exec.Start());

    no::CTaskResult<int> r =
        exec.Submit([]() { std::printf("⑧ void 任务执行\n"); }) // CTask<void>
            .Then([]() { return 42; })                            // void → int
            .Then([](int n) { return n + 8; })                    // int → int
            .Get();
    ASSERT(r.HasValue());
    ASSERT(r.Value() == 50);        // 42+8
    std::printf("   void→值 链式: %d\n", r.Value());
    exec.Stop();
}

// ⑨ OnSuccess / OnNone 回调（fire-and-forget，不阻塞）
void DemoCallbacks()
{
    no::CAsyncExecutor exec(1);
    ASSERT(exec.Start());

    no::CTask<int> t = exec.Submit([]() { return 7; });
    t.OnSuccess([](const int& v) {
        std::printf("⑨ OnSuccess: %d\n", v);
    });
    t.OnNone([](no::detail::CTaskEndReason reason) {
        std::printf("   OnNone: 链终止 reason=%d\n", static_cast<int>(reason));
    });
    no::CTaskResult<int> r = t.Get();
    ASSERT(r.HasValue());
    ASSERT(r.Value() == 7);
    exec.Stop();
}

// ⑪ 用 std::shared_ptr<T> 在链中传大对象（拷指针不拷内容）
void DemoSharedPtrChain()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

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
    std::printf("⑪ shared_ptr 链式传递: %lld（零内容拷贝）\n",
                static_cast<long long>(r.Value()));
    exec.Stop();
}

// ⑫ 并发多任务：一批任务 + 多线程 Get
void DemoConcurrent()
{
    no::CAsyncExecutor exec(4);
    ASSERT(exec.Start());

    std::vector<no::CTask<int> > tasks;
    for (int i = 0; i < 8; ++i)
    {
        tasks.push_back(exec.Submit([i]() { return i * i; }));
    }
    std::atomic<int> nOk(0);
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
    {
        threads.push_back(std::thread([&tasks, i, &nOk]() {
            no::CTaskResult<int> r = tasks[i].Get();
            if (r.HasValue() && r.Value() == i * i)
            {
                nOk.fetch_add(1);
            }
        }));
    }
    for (size_t i = 0; i < threads.size(); ++i)
    {
        threads[i].join();
    }
    ASSERT(nOk.load() == 8);
    std::printf("⑫ 并发多任务: %d/8 全部正确\n", nOk.load());
    exec.Stop();
}

// ⑬ 生命周期加固：执行器析构后，已投递任务仍安全完成（无悬垂）
void DemoLifetime()
{
    // 空任务构造已私有化，用占位执行器 Submit 创建占位任务（未启动 → 立即无值完成）。
    no::CAsyncExecutor execPlaceholder(1);
    no::CTask<int> task = execPlaceholder.Submit([]() { return 0; });
    {
        no::CAsyncExecutor exec(1);
        ASSERT(exec.Start());
        task = exec.Submit([]() { return 7; });
    } // exec 析构：Stop 等待任务完成，句柄仍被 task 持有
    no::CTaskResult<int> r = task.Get();
    ASSERT(r.HasValue());
    ASSERT(r.Value() == 7);
    std::printf("⑬ 生命周期加固: 执行器已析构，任务仍完成 = %d\n", r.Value());
}

// ⑭ Post：提交无返回值任务（fire-and-forget）
void DemoPost()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::atomic<int> nDone(0);
    bool bPosted = exec.Post([&nDone]() { nDone.fetch_add(1); });
    ASSERT(bPosted);              // 提交成功
    exec.Stop();                  // Stop 等待已提交任务完成
    ASSERT(nDone.load() == 1);    // Post 的任务已执行完
    std::printf("⑭ Post: 已执行 %d 个无返回值任务\n", nDone.load());
}

// ⑮ 执行器未启动：Submit 立即以无值（kNotStarted）完成，不抛异常
void DemoNotStarted()
{
    no::CAsyncExecutor exec(1);   // 不调用 Start()
    no::CTaskResult<int> r = exec.Submit([]() { return 1; }).Get();
    ASSERT(!r.HasValue());                       // 未启动 → 无值
    ASSERT(r.Reason() == no::detail::kNotStarted);
    std::printf("⑮ 未启动执行器: reason=%d（kNotStarted）\n",
                static_cast<int>(r.Reason()));
}

// ⑯ 类型变化链：Then 链上每级返回值类型都不同（CTask<int> → string → size_t）
void DemoTypeChangingChain()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    no::CTaskResult<std::size_t> r =
        exec.Submit([]() { return 3; })                                // CTask<int>
            .Then([](int n) { return std::to_string(n * 10); })   // → CTask<string> "30"
            .Then([](const std::string& s) { return s.size(); })  // → CTask<size_t> 2
            .Get();
    ASSERT(r.HasValue());
    ASSERT(r.Value() == static_cast<std::size_t>(2)); // "30" 的长度
    std::printf("⑯ 类型变化链: int→string→size_t，长度=%zu\n", r.Value());
    exec.Stop();
}

// ⑰ 任务源码位置（NOTHROW_LOC）：调试构建（make debug）下保存注册点
//    函数名/文件/行号；同一函数内多个调用点可用行号（__LINE__）区分。
void DemoTaskTag()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    no::CTaskResult<int> r =
        exec.Submit([]() { return 1; }, NOTHROW_LOC)
            .Then([](int n) { return n + 1; }, NOTHROW_LOC)
            .Then([](int n) { return n * 10; }, NOTHROW_LOC)
            .Get();
    ASSERT(r.HasValue());
    ASSERT(r.Value() == 20);   // (1+1)*10
    std::printf("⑰ 任务源码位置: 结果=%d（函数/文件/行号）\n", r.Value());
    exec.Stop();
}

int main()
{
    DemoSubmitAndGet();
    DemoChainThen();
    DemoTransformResult();
    DemoFlatMap();
    DemoNonePropagation();
    DemoExceptionToNone();
    DemoVoidToVoid();
    DemoVoidThenValue();
    DemoCallbacks();
    DemoSharedPtrChain();
    DemoConcurrent();
    DemoLifetime();
    DemoPost();
    DemoNotStarted();
    DemoTypeChangingChain();
    DemoTaskTag();

    if (g_nAssertFailures == 0)
    {
        std::printf("全部断言通过 ✔\n");
        return 0;
    }
    std::printf("共 %d 个断言失败\n", g_nAssertFailures);
    return 1;
}
