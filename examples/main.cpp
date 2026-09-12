// ============================================================
// 异步链（common::async 特化版）完整示例
//
// 本框架的链与「层间传任意值」的通用任务链相反：
//   - 层与层之间只传递「兑现 / 拒绝」（CPromiseResult）；
//   - 数据统一放在共享上下文（std::shared_ptr<TContext>，整条链共用同一实例）；
//   - 层函数签名固定：
//         CPromiseResult handler(CPromiseResult upResult, const std::shared_ptr<TContext>& spCtx);
//     下一层据此判断上一层回调的成败；数据一律从 spCtx 读写。
//
// 项目结构参考标准服务器项目：main.cpp（入口）+ Linux/Makefile（构建配置）。
// 构建与运行：
//   cd examples/Linux && make run            # 构建并运行（release）
//   ./build.sh examples                      # 或经统一构建脚本
//
// 用法速览（每个函数演示一类用法）：
//   ①  NewPromise + Await   起 promise 并阻塞取结果
//   ②  链式 then            多层顺序执行，数据走共享上下文
//   ③  catch 看到拒绝       upResult 携带上一层结果（仅被拒绝时执行）
//   ④  then 失败即停        后续 then 层不执行，拒绝码透传
//   ⑤  回滚 / 恢复          catch 透传拒绝 / Resolve() 吞掉拒绝继续
//   ⑥  异常转拒绝           处理器内异常 → kException，不向调用方抛出
//   ⑦  settled 通知         OnSettled（兑现 / 拒绝都触发一次）
//   ⑧  分叉                 同一层注册多个 then，各自独立延续
//   ⑨  上下文创建           链内懒创建 / 外部注入
//   ⑩  Post                 无返回值任务（fire-and-forget）
//   ⑪  并发多条链           多线程并行 + 并发 Await 同一 promise
//   ⑫  生命周期加固         执行器析构后 promise 仍安全完成
//   ⑬  未启动执行器         起 promise 立即被拒绝（kStopped）
//   ⑭  深链                 超过内联深度上限自动改投递
//   ⑮  注册点源码位置       ASYNC_LOC（调试构建保存函数 / 文件 / 行号）
//   ⑯  协程顺序 await       CO_AWAIT
//   ⑰  协程并行 await       CO_AWAIT_ALL
//   ⑱  协程 await 拒绝      协程终止，拒绝码透传
//   ⑲  嵌套协程             await 子协程 AsPromise()
//   ⑳  finally 收尾         无论兑现还是拒绝都执行，不改结果
//
//   —— 嵌套用法（异步里再起异步）——
//   ㉑  层内嵌套（阻塞）     层里起子 promise 并等它（需 ≥2 工作线程，会占住一个 worker）
//   ㉒  层内嵌套（非阻塞）   起子 promise 后返回，由它的 settled 回调接着干活（单线程也安全）
//   ㉓  层内 fire-and-forget 层里 Post 重活，链不等它（只写不被后续层碰的字段）
//   ㉔  协程跨上下文嵌套     await 另一套 TContext 的子流程（同/跨上下文、多步子 promise）
//   ㉕  并行嵌套             CO_AWAIT_ALL 里每条都是「多步子 promise + 独立上下文」
//   ㉖  分叉 + 协程汇聚      同一层分叉两条分支，协程并行等待后汇聚（纯异步的等价写法是
//                            exec.WhenAll，见 docs/common/async-usage.md）
//   ㉗  跨模块组合           new Promise 桥接别的模块的 promise + then-promise 接进本流程
//                            （纯异步、零阻塞、不用协程；单线程执行器也安全）
//   ㉘  混用多种 then        具名异步函数 / lambda / lambda 里执行其他异步函数（等它 / 不等它）
//                            （单独文件：examples/cases/ThenMixCase.cpp，一条链看全写法）
// ============================================================
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Async/AsyncExecutor.h"
#include "Async/Coroutine.h"
#include "Async/Promise.h"
#include "cases/ThenMixCase.h"

// ============================================================
// 轻量 ASSERT（示例自我校验用，MFC 命名风格）
// 条件为假时打印位置并累计失败数，不中断程序。
// ============================================================
static int g_nAssertFailures = 0;

#define ASSERT(cond)                                                                 \
    do                                                                               \
    {                                                                                \
        if (!(cond))                                                                 \
        {                                                                            \
            std::printf("  [ASSERT] 失败: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            ++g_nAssertFailures;                                                     \
        }                                                                            \
    } while (0)

// ============================================================
// 演示用共享上下文与层函数
// ============================================================

/// 演示用业务错误码（业务码从 kBusinessBase 起取）。
enum DemoCode
{
    kCodeInvalid = common::async::kBusinessBase + 1,     ///< 参数非法。
    kCodeStoreFailed = common::async::kBusinessBase + 2  ///< 落库失败。
};

/// @brief 一次流程的共享数据（TContext）：各层读写它，层间只传成败。
struct CDemoContext
{
    int nBase;                   ///< 输入基数。
    int nScaled;                 ///< 缩放结果。
    bool bFailParam;             ///< 模拟参数非法。
    bool bFailStore;             ///< 模拟落库失败。
    bool bRolledBack;            ///< 是否回滚。
    std::string strTrace;        ///< 层执行轨迹。
    std::atomic<int> nForkDone;  ///< 分叉演示：两条分支各 +1（原子，并行安全）。

    CDemoContext() : nBase(10), nScaled(0), bFailParam(false), bFailStore(false), bRolledBack(false), nForkDone(0)
    {}
};

/// 层：读参数。
static common::async::CPromiseResult StepReadParam(
    common::async::CPromiseResult upResult, const std::shared_ptr<CDemoContext>& spCtx)
{
    if (upResult.IsRejected())
    {
        return upResult;
    }
    spCtx->strTrace += "读参数;";
    return spCtx->bFailParam ? common::async::CPromiseResult::Reject(kCodeInvalid)
                             : common::async::CPromiseResult::Resolve();
}

/// 层：缩放（基数 ×3）。
static common::async::CPromiseResult StepScale(
    common::async::CPromiseResult upResult, const std::shared_ptr<CDemoContext>& spCtx)
{
    if (upResult.IsRejected())
    {
        return upResult;
    }
    spCtx->nScaled = spCtx->nBase * 3;
    spCtx->strTrace += "缩放;";
    return common::async::CPromiseResult::Resolve();
}

/// 层：落库（可模拟失败）。
static common::async::CPromiseResult StepStore(
    common::async::CPromiseResult upResult, const std::shared_ptr<CDemoContext>& spCtx)
{
    if (upResult.IsRejected())
    {
        return upResult;
    }
    spCtx->strTrace += "落库;";
    return spCtx->bFailStore ? common::async::CPromiseResult::Reject(kCodeStoreFailed)
                             : common::async::CPromiseResult::Resolve();
}

/// 层：被调用即留下痕迹（用于验证失败即停时未被执行）。
static common::async::CPromiseResult StepCleanup(
    common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CDemoContext>& spCtx)
{
    spCtx->strTrace += "清理;";
    return common::async::CPromiseResult::Resolve();
}

/// 处理器（catch）：回滚——仅在上一层被拒绝时执行，并看得到拒绝结果。
static common::async::CPromiseResult StepRollback(
    common::async::CPromiseResult upResult, const std::shared_ptr<CDemoContext>& spCtx)
{
    spCtx->strTrace += "回滚;";
    if (upResult.IsRejected())
    {
        spCtx->bRolledBack = true;
        return upResult;  // 透传失败：后续 Then 层仍不执行。
    }
    return common::async::CPromiseResult::Resolve();
}

/// 处理器（catch）：吞掉拒绝（补偿后恢复链）。
static common::async::CPromiseResult StepRecover(
    common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CDemoContext>& spCtx)
{
    spCtx->strTrace += "恢复;";
    return common::async::CPromiseResult::Resolve();  // Resolve() → 吞掉拒绝，promise 从本层之后继续。
}

/// 处理器：抛异常（框架捕获 → 本层被拒绝 kException）。
static common::async::CPromiseResult StepThrow(
    common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CDemoContext>& /*spCtx*/)
{
    throw std::runtime_error("演示用异常");
}

// ---------------- 嵌套演示用的上下文与处理器（子流程 / 并行分支） ----------------

/// @brief 子流程上下文：与父流程的 CDemoContext **不同**，演示跨上下文嵌套。
struct CSubContext
{
    int nRows;             ///< 查询到的行数。
    std::string strTrace;  ///< 子流程轨迹。

    CSubContext() : nRows(0)
    {}
};

/// 处理器（子流程）：模拟一次查询（3 行，耗时 5ms）。
static common::async::CPromiseResult StepQueryRows(
    common::async::CPromiseResult upResult, const std::shared_ptr<CSubContext>& spCtx)
{
    if (upResult.IsRejected())
    {
        return upResult;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));  // 模拟 IO
    spCtx->nRows = 3;
    spCtx->strTrace += "查询;";
    return common::async::CPromiseResult::Resolve();
}

/// @brief 分支上下文：每条并行分支一个独立实例（并行写同一份数据不安全）。
struct CBranchContext
{
    int nId;     ///< 分支编号。
    int nValue;  ///< 分支结果。

    CBranchContext() : nId(0), nValue(0)
    {}
};

/// 处理器（分支第一步）：载入（+10，耗时 10ms）。
static common::async::CPromiseResult StepBranchLoad(
    common::async::CPromiseResult upResult, const std::shared_ptr<CBranchContext>& spCtx)
{
    if (upResult.IsRejected())
    {
        return upResult;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));  // 模拟 IO
    spCtx->nValue += 10;
    return common::async::CPromiseResult::Resolve();
}

/// 处理器（分支第二步）：保存（+1）—— 与上一步用 then 串成「多步并行子流程」。
static common::async::CPromiseResult StepBranchSave(
    common::async::CPromiseResult upResult, const std::shared_ptr<CBranchContext>& spCtx)
{
    if (upResult.IsRejected())
    {
        return upResult;
    }
    spCtx->nValue += 1;
    return common::async::CPromiseResult::Resolve();
}

/// 处理器（分叉分支）：只做原子计数（并行安全），不碰共享的可变数据。
static common::async::CPromiseResult StepForkBranch(
    common::async::CPromiseResult upResult, const std::shared_ptr<CDemoContext>& spCtx)
{
    if (upResult.IsRejected())
    {
        return upResult;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));  // 模拟并行工作
    spCtx->nForkDone.fetch_add(1, std::memory_order_relaxed);
    return common::async::CPromiseResult::Resolve();
}

// ① 最基本：起 promise（NewPromise）+ 阻塞取结果（Await）
void DemoNewPromiseAndAwait()
{
    common::async::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->nBase = 42;
    // ASYNC_LOC：调试构建（make debug）下保存注册点的函数 / 文件 / 行号。
    common::async::CPromiseResult r = exec.NewPromise(spCtx, &StepReadParam, ASYNC_LOC).Await();
    ASSERT(r.IsFulfilled());
    ASSERT(spCtx->nBase == 42);
    std::printf("① NewPromise + Await: 结果=%s 码=%d\n", r.IsFulfilled() ? "兑现" : "拒绝", r.Code());
    exec.Stop();
}

// ② 链式 Then：多层顺序执行；数据经共享上下文传递（层间只传成败）
void DemoChainThen()
{
    common::async::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->nBase = 10;
    common::async::CPromiseResult r = exec.NewPromise(spCtx, &StepReadParam, ASYNC_LOC)
                                          .Then(&StepScale, ASYNC_LOC)
                                          .Then(&StepStore, ASYNC_LOC)
                                          .Await();
    ASSERT(r.IsFulfilled());
    ASSERT(spCtx->nScaled == 30);                                 // 数据在上下文里
    ASSERT(spCtx->strTrace == std::string("读参数;缩放;落库;"));  // 层按注册顺序执行
    std::printf("② 链式 Then: 缩放=%d 轨迹=%s\n", spCtx->nScaled, spCtx->strTrace.c_str());
    exec.Stop();
}

// ③ catch 看到上一层拒绝：upResult 携带拒绝结果（仅被拒绝时执行）
void DemoCatchSeesRejection()
{
    common::async::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    // 落库失败 → catch 层仍执行，且 upResult.IsRejected() 为真
    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->bFailStore = true;
    common::async::CPromiseResult r = exec.NewPromise(spCtx, &StepReadParam, ASYNC_LOC)
                                          .Then(&StepScale, ASYNC_LOC)
                                          .Then(&StepStore, ASYNC_LOC)
                                          .Catch(&StepRollback, ASYNC_LOC)
                                          .Await();
    ASSERT(r.IsRejected());
    ASSERT(spCtx->bRolledBack);  // 回滚层看到了上一层失败
    ASSERT(spCtx->strTrace == std::string("读参数;缩放;落库;回滚;"));
    std::printf(
        "③ catch 看到拒绝: 回滚=%s 轨迹=%s\n", spCtx->bRolledBack ? "已执行" : "未执行", spCtx->strTrace.c_str());
    exec.Stop();
}

// ④ 失败即停：某层失败后，后续 Then 层不再执行，失败码透传到 Get()
void DemoThenFailFast()
{
    common::async::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->bFailParam = true;  // 读参数层失败
    common::async::CPromiseResult r = exec.NewPromise(spCtx, &StepReadParam, ASYNC_LOC)
                                          .Then(&StepScale, ASYNC_LOC)    // 不执行
                                          .Then(&StepStore, ASYNC_LOC)    // 不执行
                                          .Then(&StepCleanup, ASYNC_LOC)  // 不执行
                                          .Await();
    ASSERT(r.IsRejected());
    ASSERT(r.Code() == kCodeInvalid);  // 业务拒绝码原样透传
    ASSERT(spCtx->strTrace == std::string("读参数;"));
    std::printf("④ then 失败即停: 码=%d（业务码从 kBusinessBase=%d 起）轨迹=%s\n", r.Code(),
        common::async::kBusinessBase, spCtx->strTrace.c_str());
    exec.Stop();
}

// ⑤ 回滚 / 恢复：catch 返回 upResult 即继续透传拒绝；返回 Resolve() 即吞掉拒绝继续
void DemoCatchRollbackAndRecover()
{
    common::async::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    // 5.1 回滚后透传失败 → 后续层仍不执行
    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->bFailStore = true;
    common::async::CPromiseResult r1 = exec.NewPromise(spCtx, &StepReadParam, ASYNC_LOC)
                                           .Then(&StepStore, ASYNC_LOC)
                                           .Catch(&StepRollback, ASYNC_LOC)  // 透传失败
                                           .Then(&StepCleanup, ASYNC_LOC)    // 不执行
                                           .Await();
    ASSERT(r1.IsRejected());
    ASSERT(spCtx->strTrace == std::string("读参数;落库;回滚;"));

    // 5.2 回滚后恢复 → 后续层重新执行
    std::shared_ptr<CDemoContext> spCtx2 = std::make_shared<CDemoContext>();
    spCtx2->bFailStore = true;
    common::async::CPromiseResult r2 = exec.NewPromise(spCtx2, &StepReadParam, ASYNC_LOC)
                                           .Then(&StepStore, ASYNC_LOC)
                                           .Catch(&StepRecover, ASYNC_LOC)  // 吞掉失败
                                           .Then(&StepCleanup, ASYNC_LOC)   // 执行
                                           .Await();
    ASSERT(r2.IsFulfilled());  // 链已恢复
    ASSERT(spCtx2->strTrace == std::string("读参数;落库;恢复;清理;"));
    std::printf("⑤ 回滚 / 恢复: 透传=%s 恢复后=%s\n", spCtx->strTrace.c_str(), spCtx2->strTrace.c_str());
    exec.Stop();
}

// ⑥ 处理器内异常 → 本层被拒绝（kException），框架不向调用方抛出
void DemoExceptionToFailed()
{
    common::async::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    common::async::CPromiseResult r = exec.NewPromise(spCtx, &StepThrow, ASYNC_LOC)
                                          .Then(&StepCleanup, ASYNC_LOC)  // 不执行
                                          .Await();
    ASSERT(r.IsRejected());
    ASSERT(r.Code() == static_cast<int>(common::async::kException));
    ASSERT(spCtx->strTrace.empty());
    std::printf("⑥ 异常转拒绝: 码=%d（kException）\n", r.Code());
    exec.Stop();
}

// ⑦ settled 通知 OnSettled：兑现 / 拒绝都触发一次（携带最终结果）
void DemoOnSettledCallback()
{
    common::async::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->bFailStore = true;
    std::atomic<int> nCode(-1);
    std::atomic<bool> bDone(false);

    common::async::CPromise<CDemoContext> chain = exec.NewPromise(spCtx, &StepStore, ASYNC_LOC);
    ASSERT(chain.OnSettled(
        [&nCode, &bDone](common::async::CPromiseResult r)
        {
            nCode.store(r.Code());
            bDone.store(true);
        }));
    chain.Await();
    while (!bDone.load())
    {
        std::this_thread::yield();  // 完成回调可能略晚于 Get 返回
    }
    ASSERT(nCode.load() == kCodeStoreFailed);
    std::printf("⑦ settled 通知: 码=%d\n", nCode.load());
    exec.Stop();
}

// ⑧ 分叉：同一层注册多个 Then，各自独立延续（共享同一上下文）
void DemoFork()
{
    common::async::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    common::async::CPromise<CDemoContext> head = exec.NewPromise(spCtx, &StepReadParam, ASYNC_LOC);

    std::atomic<int> nDone(0);
    common::async::CPromise<CDemoContext> branchA = head.Then(&StepScale, ASYNC_LOC);
    common::async::CPromise<CDemoContext> branchB = head.Then(&StepStore, ASYNC_LOC);
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

    ASSERT(branchA.Await().IsFulfilled());
    ASSERT(branchB.Await().IsFulfilled());
    while (nDone.load() < 2)
    {
        std::this_thread::yield();
    }
    ASSERT(spCtx->nScaled == 30);
    std::printf("⑧ 分叉: 两条分支都完成，上下文共享，轨迹=%s\n", spCtx->strTrace.c_str());
    exec.Stop();
}

// ⑨ 上下文创建：链内懒创建（GetContext）或外部注入（构造传入）
void DemoContextCreation()
{
    common::async::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    // 9.1 链内懒创建：先取上下文填初始数据，再起 promise
    common::async::CPromise<CDemoContext> chain(exec);
    ASSERT(chain.GetContext() != nullptr);  // 懒创建，恒非空
    chain.GetContext()->nBase = 20;
    common::async::CPromise<CDemoContext> tail = chain.Then(&StepReadParam, ASYNC_LOC).Then(&StepScale, ASYNC_LOC);
    ASSERT(tail.Await().IsFulfilled());
    ASSERT(chain.GetContext()->nScaled == 60);

    // 9.2 外部注入：链内所有层共用外部实例（不做拷贝）
    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->nBase = 5;
    common::async::CPromise<CDemoContext> chain2(exec, spCtx);
    ASSERT(chain2.GetContext() == spCtx);
    ASSERT(chain2.Then(&StepScale, ASYNC_LOC).Await().IsFulfilled());
    ASSERT(spCtx->nScaled == 15);
    std::printf("⑨ 上下文: 懒创建=%d 外部注入=%d\n", chain.GetContext()->nScaled, spCtx->nScaled);
    exec.Stop();
}

// ⑩ Post：无返回值任务（fire-and-forget）
void DemoPost()
{
    common::async::CAsyncExecutor exec(2);
    ASSERT(!exec.Post(
        []()
        {
        }));  // 未启动：拒绝

    ASSERT(exec.Start());
    std::atomic<int> nDone(0);
    ASSERT(exec.Post(
        [&nDone]()
        {
            nDone.fetch_add(1);
        }));
    exec.Stop();  // 等待任务完成
    ASSERT(nDone.load() == 1);
    ASSERT(!exec.Post(
        [&nDone]()
        {
            nDone.fetch_add(1);
        }));  // 已停止：拒绝
    std::printf("⑩ Post: 完成=%d（未启动 / 已停止均被拒绝）\n", nDone.load());
}

// ⑪ 并发多条链 + 并发 Await 同一 promise
void DemoConcurrent()
{
    common::async::CAsyncExecutor exec(4);
    ASSERT(exec.Start());

    // 11.1 多条链并行（各链独立上下文）
    const int kChains = 16;
    std::vector<common::async::CPromise<CDemoContext> > chains;
    for (int i = 0; i < kChains; ++i)
    {
        std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
        spCtx->nBase = i + 1;
        chains.push_back(exec.NewPromise(spCtx, &StepReadParam, ASYNC_LOC).Then(&StepScale, ASYNC_LOC));
    }
    int nOk = 0;
    for (size_t i = 0; i < chains.size(); ++i)
    {
        if (chains[i].Await().IsFulfilled())
        {
            ++nOk;
        }
    }
    ASSERT(nOk == kChains);

    // 11.2 多线程 Get 同一链（notify_all 唤醒全部等待者）
    std::shared_ptr<CDemoContext> spShared = std::make_shared<CDemoContext>();
    common::async::CPromise<CDemoContext> shared =
        exec.NewPromise(spShared, &StepReadParam, ASYNC_LOC).Then(&StepScale, ASYNC_LOC);
    std::atomic<int> nGot(0);
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
    {
        threads.push_back(std::thread(
            [&shared, &nGot]()
            {
                if (shared.Await().IsFulfilled())
                {
                    nGot.fetch_add(1);
                }
            }));
    }
    for (size_t i = 0; i < threads.size(); ++i)
    {
        threads[i].join();
    }
    ASSERT(nGot.load() == 8);
    ASSERT(spShared->strTrace == std::string("读参数;缩放;"));  // 层不会被重复执行
    std::printf("⑪ 并发: promise=%d 并发 Await=%d\n", nOk, nGot.load());
    exec.Stop();
}

// ⑫ 生命周期加固：执行器析构后，已起的 promise 仍安全完成
void DemoLifetime()
{
    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    common::async::CPromise<CDemoContext> tail;
    {
        common::async::CAsyncExecutor exec(2);
        ASSERT(exec.Start());
        tail = exec.NewPromise(spCtx, &StepReadParam, ASYNC_LOC).Then(&StepScale, ASYNC_LOC);
        exec.Stop();  // 停止并等待已投递任务完成
    }  // 执行器析构：链通过共享句柄保活线程池，不悬垂

    ASSERT(tail.Await().IsFulfilled());
    ASSERT(spCtx->nScaled == 30);
    std::printf("⑫ 生命周期: 执行器析构后仍完成，缩放=%d\n", spCtx->nScaled);
}

// ⑬ 未启动执行器：起 promise 立即被拒绝（kStopped），Await 不阻塞
void DemoNotStarted()
{
    common::async::CAsyncExecutor exec(2);
    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    common::async::CPromiseResult r =
        exec.NewPromise(spCtx, &StepReadParam, ASYNC_LOC).Then(&StepScale, ASYNC_LOC).Await();
    ASSERT(r.IsRejected());
    ASSERT(r.Code() == static_cast<int>(common::async::kStopped));
    ASSERT(spCtx->strTrace.empty());
    std::printf("⑬ 未启动执行器: 码=%d（kStopped）\n", r.Code());
}

// ⑭ 深链：层数超过内联深度上限时框架自动改投递（防递归爆栈）
void DemoDeepChain()
{
    common::async::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    const int kLayers = 256;  // > detail::kMaxInlineDepth（64）
    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    common::async::CPromise<CDemoContext> tail = exec.NewPromise(spCtx, &StepReadParam, ASYNC_LOC);
    for (int i = 0; i < kLayers; ++i)
    {
        tail = tail.Then(&StepScale, ASYNC_LOC);
    }
    ASSERT(tail.Await().IsFulfilled());
    ASSERT(spCtx->nScaled == 30);  // 缩放层重复执行，结果不变
    std::printf("⑭ 深链: %d 层全部执行完成\n", kLayers + 1);
    exec.Stop();
}

// ⑮ 注册点源码位置（ASYNC_LOC）：调试构建保存注册点函数 / 文件 / 行号
void DemoSourceLoc()
{
    common::async::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    common::async::CPromise<CDemoContext> tail =
        exec.NewPromise(spCtx, &StepReadParam, ASYNC_LOC).Then(&StepScale, ASYNC_LOC);
    const common::async::CSourceLoc loc = tail.Loc();
    (void)loc;  // 调试构建下：loc.szFunction / szFile / nLine 指向注册点
    std::printf("⑮ 注册点源码位置: 调试构建下 tail.Loc() = %s:%d\n", loc.szFile != NULL ? loc.szFile : "(发布构建为空)",
        loc.nLine);
    ASSERT(tail.Await().IsFulfilled());
    exec.Stop();
}

// ---------------- 协程（用顺序代码 await 多条链） ----------------

/// 协程：顺序 await 三条子链（子链与协程共享同一上下文）。
class CDemoCoroutine : public common::async::CCoroutine<CDemoContext>
{
public:
    using common::async::CCoroutine<CDemoContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(&StepReadParam));
        CO_AWAIT(NewPromise(&StepScale));
        CO_AWAIT(NewPromise(&StepStore));
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 协程：并行 await 多条子链。
class CParallelCoroutine : public common::async::CCoroutine<CDemoContext>
{
public:
    using common::async::CCoroutine<CDemoContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_ALL(NewPromise(&StepReadParam), NewPromise(&StepScale), NewPromise(&StepStore));
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 协程：await 到失败 → 终止（失败码透传，后续 await 不执行）。
class CFailCoroutine : public common::async::CCoroutine<CDemoContext>
{
public:
    using common::async::CCoroutine<CDemoContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(&StepReadParam));
        CO_AWAIT(NewPromise(&StepStore));    // 失败
        CO_AWAIT(NewPromise(&StepCleanup));  // 不执行
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 子协程：await 一条子链。
class CChildCoroutine : public common::async::CCoroutine<CDemoContext>
{
public:
    using common::async::CCoroutine<CDemoContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(&StepScale));
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 父协程：await 子协程（嵌套）。
class CParentCoroutine : public common::async::CCoroutine<CDemoContext>
{
public:
    explicit CParentCoroutine(const std::shared_ptr<CDemoContext>& spCtx, common::async::CAsyncExecutor* pExec)
        : common::async::CCoroutine<CDemoContext>(spCtx), m_pExec(pExec), m_pChild()
    {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(&StepReadParam));
        m_pChild = m_pExec->CoStart<CChildCoroutine>(GetContext());  // 跨 await 的变量须为成员
        CO_AWAIT(m_pChild->AsPromise());
        CO_RETURN_VOID();
        CO_END();
    }

private:
    common::async::CAsyncExecutor* m_pExec;
    std::shared_ptr<CChildCoroutine> m_pChild;
};

// ⑯ 协程顺序 await：CO_AWAIT
void DemoCoroutineSequential()
{
    common::async::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->nBase = 7;
    std::shared_ptr<CDemoCoroutine> pCoro = exec.CoStart<CDemoCoroutine>(spCtx);
    const common::async::CPromiseResult r = pCoro->Await();
    ASSERT(r.IsFulfilled());
    ASSERT(pCoro->GetContext() == spCtx);  // 协程与子链共享同一上下文
    ASSERT(spCtx->nScaled == 21);
    ASSERT(spCtx->strTrace == std::string("读参数;缩放;落库;"));
    std::printf("⑯ 协程顺序 await: 缩放=%d 轨迹=%s\n", spCtx->nScaled, spCtx->strTrace.c_str());
    exec.Stop();
}

// ⑰ 协程并行 await：CO_AWAIT_ALL
void DemoCoroutineParallel()
{
    common::async::CAsyncExecutor exec(4);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->nBase = 7;
    std::shared_ptr<CParallelCoroutine> pCoro = exec.CoStart<CParallelCoroutine>(spCtx);
    ASSERT(pCoro->Await().IsFulfilled());
    ASSERT(spCtx->nScaled == 21);  // 数据在上下文里（顺序不定）
    std::printf("⑰ 协程并行 await: 缩放=%d 轨迹=%s\n", spCtx->nScaled, spCtx->strTrace.c_str());
    exec.Stop();
}

// ⑱ 协程 await 拒绝：协程终止，拒绝码透传，后续 await 不执行
void DemoCoroutineAwaitRejected()
{
    common::async::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->bFailStore = true;
    std::shared_ptr<CFailCoroutine> pCoro = exec.CoStart<CFailCoroutine>(spCtx);
    const common::async::CPromiseResult r = pCoro->Await();
    ASSERT(r.IsRejected());
    ASSERT(r.Code() == kCodeStoreFailed);
    ASSERT(spCtx->strTrace == std::string("读参数;落库;"));  // 清理层未执行
    std::printf("⑱ 协程 await 拒绝: 码=%d 轨迹=%s\n", r.Code(), spCtx->strTrace.c_str());
    exec.Stop();
}

// ⑲ 嵌套协程：await 子协程（AsPromise），共用同一上下文
void DemoCoroutineNested()
{
    common::async::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->nBase = 4;
    std::shared_ptr<CParentCoroutine> pCoro = exec.CoStart<CParentCoroutine>(spCtx, &exec);
    const common::async::CPromiseResult r = pCoro->Await();
    ASSERT(r.IsFulfilled());
    ASSERT(spCtx->nScaled == 12);
    ASSERT(spCtx->strTrace == std::string("读参数;缩放;"));
    std::printf("⑲ 嵌套协程: 缩放=%d 轨迹=%s\n", spCtx->nScaled, spCtx->strTrace.c_str());
    exec.Stop();
}

// ---------------- 嵌套用法（异步里再起异步） ----------------

// ㉑ 层内嵌套（阻塞）：层里起一条子 promise 并**等它结束**。
//     - 子流程可以用**另一套上下文**（CSubContext），直接复用已有处理器；
//     - 代价：会占住一个工作线程，所以线程池必须还有空闲 worker —— 单线程执行器必死锁。
void DemoNestedBlockingInLayer()
{
    common::async::CAsyncExecutor exec(2);  // 关键前提：≥ 2 个工作线程
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    std::shared_ptr<CSubContext> spSub = std::make_shared<CSubContext>();

    const common::async::CPromiseResult r =
        exec.NewPromise(
                spCtx,
                [&exec, spSub](common::async::CPromiseResult upResult,
                    const std::shared_ptr<CDemoContext>& sp) -> common::async::CPromiseResult
                {
                    if (upResult.IsRejected())
                    {
                        return upResult;
                    }
                    // 层内嵌套：起子 promise（另一套上下文）并阻塞等它结束
                    const common::async::CPromiseResult sub = exec.NewPromise(spSub, &StepQueryRows, ASYNC_LOC).Await();
                    if (sub.IsRejected())
                    {
                        return sub;  // 子流程被拒绝 → 本层拒绝（拒绝码向上透传）
                    }
                    sp->nScaled = spSub->nRows;  // 子流程数据写回父上下文
                    sp->strTrace += "父层;";
                    return common::async::CPromiseResult::Resolve();
                },
                ASYNC_LOC)
            .Await();

    ASSERT(r.IsFulfilled());
    ASSERT(spSub->nRows == 3);
    ASSERT(spCtx->nScaled == 3);
    ASSERT(spSub->strTrace == std::string("查询;"));
    std::printf("㉑ 层内阻塞嵌套: 子流程行数=%d 父上下文缩放=%d\n", spSub->nRows, spCtx->nScaled);
    exec.Stop();
}

// ㉒ 层内嵌套（非阻塞，回调驱动）：层里起子 promise 后立刻返回，由它的 settled 回调接着干活。
//     - 不占线程，**单线程执行器也安全**；
//     - 回调与后续层可能并发 —— 让它只写不被后续层碰的字段（或自行加同步）。
void DemoNestedCallbackDrivenInLayer()
{
    common::async::CAsyncExecutor exec(1);  // 单线程也安全：全程不阻塞
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    std::shared_ptr<CSubContext> spSub = std::make_shared<CSubContext>();
    std::atomic<bool> bSubDone(false);

    common::async::CPromise<CDemoContext> outer = exec.NewPromise(
        spCtx,
        [&exec, spSub, &bSubDone](common::async::CPromiseResult upResult,
            const std::shared_ptr<CDemoContext>& sp) -> common::async::CPromiseResult
        {
            if (upResult.IsRejected())
            {
                return upResult;
            }
            // 起子 promise 但**不等待**：由它的 settled 回调继续（回调驱动嵌套）
            exec.NewPromise(spSub, &StepQueryRows, ASYNC_LOC)
                .OnSettled(
                    [sp, spSub, &bSubDone](common::async::CPromiseResult sub)
                    {
                        sp->nScaled = sub.IsFulfilled() ? spSub->nRows : -1;  // 回调里写父上下文
                        bSubDone.store(true);
                    });
            sp->strTrace += "父层起步;";
            return common::async::CPromiseResult::Resolve();  // 外层立刻继续，不等子流程
        },
        ASYNC_LOC);

    ASSERT(outer.Await().IsFulfilled());
    while (!bSubDone.load())
    {
        std::this_thread::yield();  // 等子流程回调（演示用；真实业务无需等待）
    }
    ASSERT(spCtx->nScaled == 3);
    std::printf("㉒ 层内非阻塞嵌套: 父链已结束，子流程回调随后写入 缩放=%d\n", spCtx->nScaled);
    exec.Stop();
}

// ㉓ 层内 fire-and-forget：把重活 Post 出去，链继续（不等结果）。
void DemoNestedPostInLayer()
{
    common::async::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    std::atomic<bool> bDone(false);

    const common::async::CPromiseResult r =
        exec.NewPromise(
                spCtx,
                [&exec, &bDone](common::async::CPromiseResult upResult,
                    const std::shared_ptr<CDemoContext>& sp) -> common::async::CPromiseResult
                {
                    if (upResult.IsRejected())
                    {
                        return upResult;
                    }
                    // 重活下沉：Post 到工作线程，链不等它（fire-and-forget）
                    const bool bOk = exec.Post(
                        [sp, &bDone]()
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            sp->nScaled = 99;  // 只写后续层不碰的字段（否则需自行同步）
                            bDone.store(true);
                        });
                    return bOk ? common::async::CPromiseResult::Resolve()
                               : common::async::CPromiseResult::Reject(common::async::kStopped);
                },
                ASYNC_LOC)
            .Await();

    ASSERT(r.IsFulfilled());
    while (!bDone.load())
    {
        std::this_thread::yield();
    }
    ASSERT(spCtx->nScaled == 99);
    std::printf("㉓ 层内 Post: 链立即完成，后台任务稍后写入 缩放=%d\n", spCtx->nScaled);
    exec.Stop();
}

/// 协程：同上下文子 promise + **另一套上下文**的子 promise + 多步子 promise（嵌套）。
class CNestedContextCoroutine : public common::async::CCoroutine<CDemoContext>
{
public:
    explicit CNestedContextCoroutine(const std::shared_ptr<CDemoContext>& spCtx, common::async::CAsyncExecutor* pExec)
        : common::async::CCoroutine<CDemoContext>(spCtx), m_pExec(pExec), m_spSub()
    {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(&StepReadParam));                               // 同上下文子 promise
        m_spSub = std::make_shared<CSubContext>();                          // 跨 await → 成员变量
        CO_AWAIT(m_pExec->NewPromise(m_spSub, &StepQueryRows, ASYNC_LOC));  // 跨上下文 await
        CO_AWAIT(NewPromise(&StepScale).Then(&StepStore));                  // 多步子 promise（then 串联）
        GetContext()->nScaled += m_spSub->nRows;                            // 恢复后把子流程结果并入
        CO_RETURN_VOID();
        CO_END();
    }

private:
    common::async::CAsyncExecutor* m_pExec;
    std::shared_ptr<CSubContext> m_spSub;
};

// ㉔ 协程跳上下文嵌套：一次跑完「同上下文子 promise + 另上下文子流程 + 多步子 promise」。
void DemoCoroutineNestedContext()
{
    common::async::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    std::shared_ptr<CNestedContextCoroutine> pCoro = exec.CoStart<CNestedContextCoroutine>(spCtx, &exec);
    const common::async::CPromiseResult r = pCoro->Await();

    ASSERT(r.IsFulfilled());
    ASSERT(spCtx->nScaled == 33);  // 缩放 30 + 子流程 3 行
    ASSERT(spCtx->strTrace == std::string("读参数;缩放;落库;"));
    std::printf("㉔ 协程跨上下文嵌套: 缩放=%d（含子流程 3 行）轨迹=%s\n", spCtx->nScaled, spCtx->strTrace.c_str());
    exec.Stop();
}

/// 协程：并行 await 三条**多步**子 promise（每条自带独立上下文，避免并行写竞争）。
class CParallelNestedCoroutine : public common::async::CCoroutine<CDemoContext>
{
public:
    explicit CParallelNestedCoroutine(const std::shared_ptr<CDemoContext>& spCtx, common::async::CAsyncExecutor* pExec)
        : common::async::CCoroutine<CDemoContext>(spCtx), m_pExec(pExec), m_vecBranches()
    {}

    void Run() override
    {
        CO_BEGIN();
        for (int i = 0; i < 3; ++i)  // 三条分支各一份独立上下文（跨 await → 存成员保活）
        {
            std::shared_ptr<CBranchContext> spBranch = std::make_shared<CBranchContext>();
            spBranch->nId = i;
            m_vecBranches.push_back(spBranch);
        }
        CO_AWAIT_ALL(m_pExec->NewPromise(m_vecBranches[0], &StepBranchLoad, ASYNC_LOC).Then(&StepBranchSave, ASYNC_LOC),
            m_pExec->NewPromise(m_vecBranches[1], &StepBranchLoad, ASYNC_LOC).Then(&StepBranchSave, ASYNC_LOC),
            m_pExec->NewPromise(m_vecBranches[2], &StepBranchLoad, ASYNC_LOC).Then(&StepBranchSave, ASYNC_LOC));

        int nTotal = 0;  // 恢复后汇入父上下文
        for (size_t i = 0; i < m_vecBranches.size(); ++i)
        {
            nTotal += m_vecBranches[i]->nValue;
        }
        GetContext()->nScaled = nTotal;
        CO_RETURN_VOID();
        CO_END();
    }

private:
    common::async::CAsyncExecutor* m_pExec;
    std::vector<std::shared_ptr<CBranchContext> > m_vecBranches;
};

// ㉕ 并行嵌套：CO_AWAIT_ALL 里每一条都是「多步子 promise + 独立上下文」。
void DemoCoroutineParallelNested()
{
    common::async::CAsyncExecutor exec(4);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    std::shared_ptr<CParallelNestedCoroutine> pCoro = exec.CoStart<CParallelNestedCoroutine>(spCtx, &exec);
    const common::async::CPromiseResult r = pCoro->Await();

    ASSERT(r.IsFulfilled());
    ASSERT(spCtx->nScaled == 33);  // 三条分支各 11（载入 10 + 保存 1）
    std::printf("㉕ 并行嵌套: 三条多步分支汇聚=%d\n", spCtx->nScaled);
    exec.Stop();
}

/// 协程：汇聚同一层分叉出的两条分支（用协程并行 await；纯异步场景可直接用 exec.WhenAll）。
class CFanInCoroutine : public common::async::CCoroutine<CDemoContext>
{
public:
    explicit CFanInCoroutine(const std::shared_ptr<CDemoContext>& spCtx, common::async::CAsyncExecutor* pExec)
        : common::async::CCoroutine<CDemoContext>(spCtx), m_pExec(pExec), m_head(), m_branchA(), m_branchB()
    {}

    void Run() override
    {
        CO_BEGIN();
        m_head = m_pExec->NewPromise(GetContext(), &StepReadParam, ASYNC_LOC);  // 公共前段
        m_branchA = m_head.Then(&StepForkBranch, ASYNC_LOC);                    // 分支 A
        m_branchB = m_head.Then(&StepForkBranch, ASYNC_LOC);                    // 分支 B
        CO_AWAIT_ALL(m_branchA, m_branchB);                                     // 汇聚：等两条分支都结束
        CO_RETURN_VOID();
        CO_END();
    }

private:
    common::async::CAsyncExecutor* m_pExec;
    common::async::CPromise<CDemoContext> m_head;
    common::async::CPromise<CDemoContext> m_branchA;
    common::async::CPromise<CDemoContext> m_branchB;
};

// ㉖ 分叉 + 协程汇聚：两条分支并行跑，协程等全部结束（分支只写原子计数，并行安全）。
void DemoForkAndCoroutineFanIn()
{
    common::async::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    std::shared_ptr<CFanInCoroutine> pCoro = exec.CoStart<CFanInCoroutine>(spCtx, &exec);
    const common::async::CPromiseResult r = pCoro->Await();

    ASSERT(r.IsFulfilled());
    ASSERT(spCtx->nForkDone.load() == 2);               // 两条分支都完成
    ASSERT(spCtx->strTrace == std::string("读参数;"));  // 分支只计数，不写轨迹
    std::printf("㉖ 分叉 + 协程汇聚: 两条分支完成数=%d\n", spCtx->nForkDone.load());
    exec.Stop();
}

// ==================== 跨模块组合（纯异步：new Promise + then-promise） ====================

/// 演示用业务错误码（别的模块失败）。
enum BridgeDemoCode
{
    kCodeOtherModuleFailed = common::async::kBusinessBase + 11  ///< 别的模块（子流程）失败。
};

/// @brief 「别的模块」的异步函数：返回一条 CSubContext 的 promise。
///
/// 模拟真实项目里的下游模块（数据访问 / 远程服务）：它有自己的上下文类型，
/// 对外只给一个 promise 句柄（内部几层异步与调用方无关）。
///
/// @param exec 执行器（本演示共用）。
/// @param bFail true 时子流程失败。
/// @return 子流程 promise 句柄。
static common::async::CPromise<CSubContext> QueryRowsOfOtherModule(common::async::CAsyncExecutor& exec, bool bFail)
{
    std::shared_ptr<CSubContext> spSub = std::make_shared<CSubContext>();
    return exec.NewPromise(spSub, &StepQueryRows, ASYNC_LOC)
        .Then(
            [bFail](common::async::CPromiseResult upResult, const std::shared_ptr<CSubContext>& spCtx)
            {
                if (upResult.IsRejected())
                {
                    return upResult;
                }
                if (bFail)
                {
                    spCtx->strTrace += "失败;";
                    return common::async::CPromiseResult::Reject(kCodeOtherModuleFailed);
                }
                return common::async::CPromiseResult::Resolve();
            },
            ASYNC_LOC);
}

/// @brief 桥接：把「别的模块的 promise」接进本流程（等价 JS 的 new Promise）。
///
/// executor 里发起别的模块的调用，由它的 OnSettled 回调 resolve() / reject(码) 本 promise ——
/// 全程只登记回调，不占线程、不阻塞（单线程执行器也安全）。
///
/// @param exec 执行器（本演示共用）。
/// @param spCtx 本流程上下文。
/// @param bFail true 时子流程失败。
/// @return 本流程的 promise（由子流程的回调 settle）。
static common::async::CPromise<CDemoContext> BridgeQueryOther(
    common::async::CAsyncExecutor& exec, const std::shared_ptr<CDemoContext>& spCtx, bool bFail)
{
    // executor 先赋给具名变量再用：长行不会被 clang-format 对齐撑开（见 docs/vscode-clangd-format.md）。
    common::async::CPromise<CDemoContext>::PromiseExecutor fnExecutor =
        [&exec, spCtx, bFail](const common::async::CPromise<CDemoContext>::ResolveFn& fnResolve,
            const common::async::CPromise<CDemoContext>::RejectFn& fnReject)
    {
        common::async::CPromise<CSubContext> promiseSub = QueryRowsOfOtherModule(exec, bFail);
        std::shared_ptr<CSubContext> spSub = promiseSub.GetContext();
        promiseSub.OnSettled(
            [spCtx, spSub, fnResolve, fnReject](common::async::CPromiseResult result)
            {
                if (result.IsRejected())
                {
                    fnReject(result.Code());  // 跨模块拒绝码 → 本流程拒绝码（此处原样透传）。
                    return;
                }
                spCtx->nScaled = spSub->nRows;  // 取回别的模块的数据，写进本流程上下文。
                spCtx->strTrace += "桥接;";
                fnResolve();
            });
    };
    return common::async::CPromise<CDemoContext>::New(exec, spCtx, fnExecutor, ASYNC_LOC);
}

/// ㉗ 跨模块组合：本模块的 then 链 + 别的模块的 promise（new Promise 桥接 + then-promise 接入）。
///
/// 与 ㉑/㉒ 的区别：本形态「等」的是**另一套上下文**的异步子流程，且本流程的最终结果
/// 含子流程结果 —— 不用协程，也不阻塞任何线程（exec 只有 1 个 worker 也能跑）。
void DemoBridgeOtherModule()
{
    common::async::CAsyncExecutor exec(1);  // 单线程执行器：本形态不靠占线程来等，因此安全
    ASSERT(exec.Start());

    // ① 成功路径：本模块层 → 桥接（别的模块）→ 本模块层继续跑
    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    const common::async::CPromiseResult r = exec.NewPromise(spCtx, &StepReadParam, ASYNC_LOC)
                                                .ThenPromise(
                                                    [&exec](const std::shared_ptr<CDemoContext>& sp)
                                                    {
                                                        return BridgeQueryOther(exec, sp, false);
                                                    },
                                                    ASYNC_LOC)
                                                .Then(&StepStore, ASYNC_LOC)  // 子流程结束后本模块的层继续
                                                .Await();
    ASSERT(r.IsFulfilled());
    ASSERT(spCtx->nScaled == 3);                                  // 别的模块的数据已取回本流程上下文
    ASSERT(spCtx->strTrace == std::string("读参数;桥接;落库;"));  // 本地层照常串接
    std::printf("㉗ 跨模块组合（成功）: 取回行数=%d 轨迹=%s\n", spCtx->nScaled, spCtx->strTrace.c_str());

    // ② 拒绝路径：别的模块拒绝 → 本流程 then 层不执行，catch 仍可见，拒绝码透传
    std::shared_ptr<CDemoContext> spCtx2 = std::make_shared<CDemoContext>();
    const common::async::CPromiseResult r2 = exec.NewPromise(spCtx2, &StepReadParam, ASYNC_LOC)
                                                 .ThenPromise(
                                                     [&exec](const std::shared_ptr<CDemoContext>& sp)
                                                     {
                                                         return BridgeQueryOther(exec, sp, true);
                                                     },
                                                     ASYNC_LOC)
                                                 .Then(&StepStore, ASYNC_LOC)      // 子流程被拒绝 → 不执行
                                                 .Catch(&StepRollback, ASYNC_LOC)  // catch 仍执行（回滚）
                                                 .Await();
    ASSERT(r2.IsRejected());
    ASSERT(r2.Code() == static_cast<int>(kCodeOtherModuleFailed));  // 拒绝码透传
    ASSERT(spCtx2->nScaled == 0);                                   // 落库层未执行
    ASSERT(spCtx2->strTrace == std::string("读参数;回滚;"));
    std::printf("㉗ 跨模块组合（拒绝）: 拒绝码=%d 轨迹=%s\n", r2.Code(), spCtx2->strTrace.c_str());

    exec.Stop();
}

int main()
{
    DemoNewPromiseAndAwait();
    DemoChainThen();
    DemoCatchSeesRejection();
    DemoThenFailFast();
    DemoCatchRollbackAndRecover();
    DemoExceptionToFailed();
    DemoOnSettledCallback();
    DemoFork();
    DemoContextCreation();
    DemoPost();
    DemoConcurrent();
    DemoLifetime();
    DemoNotStarted();
    DemoDeepChain();
    DemoSourceLoc();
    DemoCoroutineSequential();
    DemoCoroutineParallel();
    DemoCoroutineAwaitRejected();
    DemoCoroutineNested();
    DemoNestedBlockingInLayer();
    DemoNestedCallbackDrivenInLayer();
    DemoNestedPostInLayer();
    DemoCoroutineNestedContext();
    DemoCoroutineParallelNested();
    DemoForkAndCoroutineFanIn();
    DemoBridgeOtherModule();
    ASSERT(RunThenMixCase());  // 单独的例子：一条链里混用多种 then（见 cases/ThenMixCase.cpp）

    if (g_nAssertFailures == 0)
    {
        std::printf("全部断言通过 ✔\n");
        return 0;
    }
    std::printf("共 %d 个断言失败\n", g_nAssertFailures);
    return 1;
}
