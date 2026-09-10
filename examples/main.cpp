// ============================================================
// 异步链（common::async 特化版）完整示例
//
// 本框架的链与「层间传任意值」的通用任务链相反：
//   - 层与层之间只传递「本层成功 / 失败」（CStepResult）；
//   - 数据统一放在共享上下文（std::shared_ptr<TContext>，整条链共用同一实例）；
//   - 层函数签名固定：
//         CStepResult fn(CStepResult upStep, const std::shared_ptr<TContext>& spCtx);
//     下一层据此判断上一层回调的成败；数据一律从 spCtx 读写。
//
// 项目结构参考标准服务器项目：main.cpp（入口）+ Linux/Makefile（构建配置）。
// 构建与运行：
//   cd examples/Linux && make run            # 构建并运行（release）
//   ./build.sh examples                      # 或经统一构建脚本
//
// 用法速览（每个函数演示一类用法）：
//   ①  Submit + Get         起链并阻塞取成败
//   ②  链式 Then            多层顺序执行，数据走共享上下文
//   ③  下一层判断上一层     ThenAlways 拿到 upStep（失败也执行）
//   ④  失败即停             后续层不执行，业务失败码透传
//   ⑤  回滚 / 恢复          ThenAlways 透传失败 / 吞掉失败后继续
//   ⑥  异常转失败           层内异常 → kStepException，不向调用方抛出
//   ⑦  完成回调             OnCompleted（成功 / 失败都触发一次）
//   ⑧  分叉                 同一层注册多个 Then，各自独立延续
//   ⑨  上下文创建           链内懒创建 / 外部注入
//   ⑩  Post                 无返回值任务（fire-and-forget）
//   ⑪  并发多条链           多线程并行 + 并发 Get 同一链
//   ⑫  生命周期加固         执行器析构后链仍安全完成
//   ⑬  未启动执行器         起链立即失败（kStepStopped）
//   ⑭  深链                 超过内联深度上限自动改投递
//   ⑮  注册点源码位置       ASYNC_LOC（调试构建保存函数 / 文件 / 行号）
//   ⑯  协程顺序 await       CO_AWAIT
//   ⑰  协程并行 await       CO_AWAIT_ALL
//   ⑱  协程 await 失败      协程终止，失败码透传
//   ⑲  嵌套协程             await 子协程 AsChain()
// ============================================================
#include <atomic>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Async/AsyncChain.h"
#include "Async/AsyncExecutor.h"
#include "Async/Coroutine.h"

namespace no = common::async;

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

/// 演示用业务错误码（业务码从 kStepBusinessBase 起取）。
enum DemoCode
{
    kCodeInvalid = no::kStepBusinessBase + 1,     ///< 参数非法。
    kCodeStoreFailed = no::kStepBusinessBase + 2  ///< 落库失败。
};

/// @brief 一次流程的共享数据（TContext）：各层读写它，层间只传成败。
struct CDemoContext
{
    int nBase;             ///< 输入基数。
    int nScaled;           ///< 缩放结果。
    bool bFailParam;       ///< 模拟参数非法。
    bool bFailStore;       ///< 模拟落库失败。
    bool bRolledBack;      ///< 是否回滚。
    std::string strTrace;  ///< 层执行轨迹。

    CDemoContext() : nBase(10), nScaled(0), bFailParam(false), bFailStore(false), bRolledBack(false) {}
};

/// 层：读参数。
static no::CStepResult StepReadParam(no::CStepResult upStep, const std::shared_ptr<CDemoContext>& spCtx)
{
    if (upStep.IsFailed())
    {
        return upStep;
    }
    spCtx->strTrace += "读参数;";
    return spCtx->bFailParam ? no::CStepResult::Failed(kCodeInvalid) : no::CStepResult::Ok();
}

/// 层：缩放（基数 ×3）。
static no::CStepResult StepScale(no::CStepResult upStep, const std::shared_ptr<CDemoContext>& spCtx)
{
    if (upStep.IsFailed())
    {
        return upStep;
    }
    spCtx->nScaled = spCtx->nBase * 3;
    spCtx->strTrace += "缩放;";
    return no::CStepResult::Ok();
}

/// 层：落库（可模拟失败）。
static no::CStepResult StepStore(no::CStepResult upStep, const std::shared_ptr<CDemoContext>& spCtx)
{
    if (upStep.IsFailed())
    {
        return upStep;
    }
    spCtx->strTrace += "落库;";
    return spCtx->bFailStore ? no::CStepResult::Failed(kCodeStoreFailed) : no::CStepResult::Ok();
}

/// 层：被调用即留下痕迹（用于验证失败即停时未被执行）。
static no::CStepResult StepCleanup(no::CStepResult /*upStep*/, const std::shared_ptr<CDemoContext>& spCtx)
{
    spCtx->strTrace += "清理;";
    return no::CStepResult::Ok();
}

/// 层（ThenAlways）：回滚——失败也执行，并看得到上一层失败。
static no::CStepResult StepRollback(no::CStepResult upStep, const std::shared_ptr<CDemoContext>& spCtx)
{
    spCtx->strTrace += "回滚;";
    if (upStep.IsFailed())
    {
        spCtx->bRolledBack = true;
        return upStep;  // 透传失败：后续 Then 层仍不执行。
    }
    return no::CStepResult::Ok();
}

/// 层（ThenAlways）：吞掉失败（补偿后恢复链）。
static no::CStepResult StepRecover(no::CStepResult /*upStep*/, const std::shared_ptr<CDemoContext>& spCtx)
{
    spCtx->strTrace += "恢复;";
    return no::CStepResult::Ok();  // 返回成功 → 链从本层之后继续。
}

/// 层：抛异常（框架捕获 → 本层失败 kStepException）。
static no::CStepResult StepThrow(no::CStepResult /*upStep*/, const std::shared_ptr<CDemoContext>& /*spCtx*/)
{
    throw std::runtime_error("演示用异常");
}

// ① 最基本：起链（Submit）+ 阻塞取结果（Get）
void DemoSubmitAndGet()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->nBase = 42;
    // ASYNC_LOC：调试构建（make debug）下保存注册点的函数 / 文件 / 行号。
    no::CStepResult r = exec.Submit(spCtx, &StepReadParam, ASYNC_LOC).Get();
    ASSERT(r.IsOk());
    ASSERT(spCtx->nBase == 42);
    std::printf("① 起链 + Get: 成败=%s 码=%d\n", r.IsOk() ? "成功" : "失败", r.Code());
    exec.Stop();
}

// ② 链式 Then：多层顺序执行；数据经共享上下文传递（层间只传成败）
void DemoChainThen()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->nBase = 10;
    no::CStepResult r =
        exec.Submit(spCtx, &StepReadParam, ASYNC_LOC).Then(&StepScale, ASYNC_LOC).Then(&StepStore, ASYNC_LOC).Get();
    ASSERT(r.IsOk());
    ASSERT(spCtx->nScaled == 30);                                 // 数据在上下文里
    ASSERT(spCtx->strTrace == std::string("读参数;缩放;落库;"));  // 层按注册顺序执行
    std::printf("② 链式 Then: 缩放=%d 轨迹=%s\n", spCtx->nScaled, spCtx->strTrace.c_str());
    exec.Stop();
}

// ③ 下一层判断上一层回调：ThenAlways 会拿到 upStep（失败也执行）
void DemoUpStepToNextLayer()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    // 落库失败 → 回滚层（ThenAlways）仍执行，且 upStep.IsFailed() 为真
    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->bFailStore = true;
    no::CStepResult r = exec.Submit(spCtx, &StepReadParam, ASYNC_LOC)
                            .Then(&StepScale, ASYNC_LOC)
                            .Then(&StepStore, ASYNC_LOC)
                            .ThenAlways(&StepRollback, ASYNC_LOC)
                            .Get();
    ASSERT(r.IsFailed());
    ASSERT(spCtx->bRolledBack);  // 回滚层看到了上一层失败
    ASSERT(spCtx->strTrace == std::string("读参数;缩放;落库;回滚;"));
    std::printf("③ 上一层结果: 回滚=%s 轨迹=%s\n", spCtx->bRolledBack ? "已执行" : "未执行", spCtx->strTrace.c_str());
    exec.Stop();
}

// ④ 失败即停：某层失败后，后续 Then 层不再执行，失败码透传到 Get()
void DemoFailFast()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->bFailParam = true;  // 读参数层失败
    no::CStepResult r = exec.Submit(spCtx, &StepReadParam, ASYNC_LOC)
                            .Then(&StepScale, ASYNC_LOC)    // 不执行
                            .Then(&StepStore, ASYNC_LOC)    // 不执行
                            .Then(&StepCleanup, ASYNC_LOC)  // 不执行
                            .Get();
    ASSERT(r.IsFailed());
    ASSERT(r.Code() == kCodeInvalid);  // 业务失败码原样透传
    ASSERT(spCtx->strTrace == std::string("读参数;"));
    std::printf("④ 失败即停: 码=%d（业务码，从 kStepBusinessBase=%d 起）轨迹=%s\n", r.Code(), no::kStepBusinessBase,
                spCtx->strTrace.c_str());
    exec.Stop();
}

// ⑤ 回滚 / 恢复：ThenAlways 返回 upStep 即继续透传失败；返回成功即吞掉失败继续
void DemoRollbackAndRecover()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    // 5.1 回滚后透传失败 → 后续层仍不执行
    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->bFailStore = true;
    no::CStepResult r1 = exec.Submit(spCtx, &StepReadParam, ASYNC_LOC)
                             .Then(&StepStore, ASYNC_LOC)
                             .ThenAlways(&StepRollback, ASYNC_LOC)  // 透传失败
                             .Then(&StepCleanup, ASYNC_LOC)         // 不执行
                             .Get();
    ASSERT(r1.IsFailed());
    ASSERT(spCtx->strTrace == std::string("读参数;落库;回滚;"));

    // 5.2 回滚后恢复 → 后续层重新执行
    std::shared_ptr<CDemoContext> spCtx2 = std::make_shared<CDemoContext>();
    spCtx2->bFailStore = true;
    no::CStepResult r2 = exec.Submit(spCtx2, &StepReadParam, ASYNC_LOC)
                             .Then(&StepStore, ASYNC_LOC)
                             .ThenAlways(&StepRecover, ASYNC_LOC)  // 吞掉失败
                             .Then(&StepCleanup, ASYNC_LOC)        // 执行
                             .Get();
    ASSERT(r2.IsOk());  // 链已恢复
    ASSERT(spCtx2->strTrace == std::string("读参数;落库;恢复;清理;"));
    std::printf("⑤ 回滚 / 恢复: 透传=%s 恢复后=%s\n", spCtx->strTrace.c_str(), spCtx2->strTrace.c_str());
    exec.Stop();
}

// ⑥ 层内异常 → 本层失败（kStepException），框架不向调用方抛出
void DemoExceptionToFailed()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    no::CStepResult r = exec.Submit(spCtx, &StepThrow, ASYNC_LOC)
                            .Then(&StepCleanup, ASYNC_LOC)  // 不执行
                            .Get();
    ASSERT(r.IsFailed());
    ASSERT(r.Code() == static_cast<int>(no::kStepException));
    ASSERT(spCtx->strTrace.empty());
    std::printf("⑥ 异常转失败: 码=%d（kStepException）\n", r.Code());
    exec.Stop();
}

// ⑦ 完成回调 OnCompleted：成功 / 失败都触发一次（携带最终结果）
void DemoCompletedCallback()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->bFailStore = true;
    std::atomic<int> nCode(-1);
    std::atomic<bool> bDone(false);

    no::CAsyncChain<CDemoContext> chain = exec.Submit(spCtx, &StepStore, ASYNC_LOC);
    ASSERT(chain.OnCompleted([&nCode, &bDone](no::CStepResult r)
    {
        nCode.store(r.Code());
        bDone.store(true);
    }));
    chain.Get();
    while (!bDone.load())
    {
        std::this_thread::yield();  // 完成回调可能略晚于 Get 返回
    }
    ASSERT(nCode.load() == kCodeStoreFailed);
    std::printf("⑦ 完成回调: 码=%d\n", nCode.load());
    exec.Stop();
}

// ⑧ 分叉：同一层注册多个 Then，各自独立延续（共享同一上下文）
void DemoFork()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    no::CAsyncChain<CDemoContext> head = exec.Submit(spCtx, &StepReadParam, ASYNC_LOC);

    std::atomic<int> nDone(0);
    no::CAsyncChain<CDemoContext> branchA = head.Then(&StepScale, ASYNC_LOC);
    no::CAsyncChain<CDemoContext> branchB = head.Then(&StepStore, ASYNC_LOC);
    branchA.OnCompleted([&nDone](no::CStepResult) { nDone.fetch_add(1); });
    branchB.OnCompleted([&nDone](no::CStepResult) { nDone.fetch_add(1); });

    ASSERT(branchA.Get().IsOk());
    ASSERT(branchB.Get().IsOk());
    while (nDone.load() < 2)
    {
        std::this_thread::yield();
    }
    ASSERT(spCtx->nScaled == 30);
    std::printf("⑧ 分叉: 两条分支都完成，上下文共享，轨迹=%s\n", spCtx->strTrace.c_str());
    exec.Stop();
}

// ⑨ 上下文创建：链内懒创建（GetContext）或外部注入（构造传入）
void DemoTimelineAndContext()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    // 9.1 链内懒创建：先取上下文填初始数据，再起链
    no::CAsyncChain<CDemoContext> chain(exec);
    ASSERT(chain.GetContext() != nullptr);  // 懒创建，恒非空
    chain.GetContext()->nBase = 20;
    no::CAsyncChain<CDemoContext> tail = chain.Submit(&StepReadParam, ASYNC_LOC).Then(&StepScale, ASYNC_LOC);
    ASSERT(tail.Get().IsOk());
    ASSERT(chain.GetContext()->nScaled == 60);

    // 9.2 外部注入：链内所有层共用外部实例（不做拷贝）
    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->nBase = 5;
    no::CAsyncChain<CDemoContext> chain2(exec, spCtx);
    ASSERT(chain2.GetContext() == spCtx);
    ASSERT(chain2.Submit(&StepScale, ASYNC_LOC).Get().IsOk());
    ASSERT(spCtx->nScaled == 15);
    std::printf("⑨ 上下文: 懒创建=%d 外部注入=%d\n", chain.GetContext()->nScaled, spCtx->nScaled);
    exec.Stop();
}

// ⑩ Post：无返回值任务（fire-and-forget）
void DemoPost()
{
    no::CAsyncExecutor exec(2);
    ASSERT(!exec.Post([]() {}));  // 未启动：拒绝

    ASSERT(exec.Start());
    std::atomic<int> nDone(0);
    ASSERT(exec.Post([&nDone]() { nDone.fetch_add(1); }));
    exec.Stop();  // 等待任务完成
    ASSERT(nDone.load() == 1);
    ASSERT(!exec.Post([&nDone]() { nDone.fetch_add(1); }));  // 已停止：拒绝
    std::printf("⑩ Post: 完成=%d（未启动 / 已停止均被拒绝）\n", nDone.load());
}

// ⑪ 并发多条链 + 并发 Get 同一链
void DemoConcurrent()
{
    no::CAsyncExecutor exec(4);
    ASSERT(exec.Start());

    // 11.1 多条链并行（各链独立上下文）
    const int kChains = 16;
    std::vector<no::CAsyncChain<CDemoContext> > chains;
    for (int i = 0; i < kChains; ++i)
    {
        std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
        spCtx->nBase = i + 1;
        chains.push_back(exec.Submit(spCtx, &StepReadParam, ASYNC_LOC).Then(&StepScale, ASYNC_LOC));
    }
    int nOk = 0;
    for (size_t i = 0; i < chains.size(); ++i)
    {
        if (chains[i].Get().IsOk())
        {
            ++nOk;
        }
    }
    ASSERT(nOk == kChains);

    // 11.2 多线程 Get 同一链（notify_all 唤醒全部等待者）
    std::shared_ptr<CDemoContext> spShared = std::make_shared<CDemoContext>();
    no::CAsyncChain<CDemoContext> shared = exec.Submit(spShared, &StepReadParam, ASYNC_LOC).Then(&StepScale, ASYNC_LOC);
    std::atomic<int> nGot(0);
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
    {
        threads.push_back(std::thread([&shared, &nGot]()
        {
            if (shared.Get().IsOk())
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
    std::printf("⑪ 并发: 链=%d 并发Get=%d\n", nOk, nGot.load());
    exec.Stop();
}

// ⑫ 生命周期加固：执行器析构后，已起的链仍安全完成
void DemoLifetime()
{
    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    no::CAsyncChain<CDemoContext> tail;
    {
        no::CAsyncExecutor exec(2);
        ASSERT(exec.Start());
        tail = exec.Submit(spCtx, &StepReadParam, ASYNC_LOC).Then(&StepScale, ASYNC_LOC);
        exec.Stop();  // 停止并等待已投递任务完成
    }  // 执行器析构：链通过共享句柄保活线程池，不悬垂

    ASSERT(tail.Get().IsOk());
    ASSERT(spCtx->nScaled == 30);
    std::printf("⑫ 生命周期: 执行器析构后仍完成，缩放=%d\n", spCtx->nScaled);
}

// ⑬ 未启动执行器：起链立即失败（kStepStopped），Get 不阻塞
void DemoNotStarted()
{
    no::CAsyncExecutor exec(2);
    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    no::CStepResult r = exec.Submit(spCtx, &StepReadParam, ASYNC_LOC).Then(&StepScale, ASYNC_LOC).Get();
    ASSERT(r.IsFailed());
    ASSERT(r.Code() == static_cast<int>(no::kStepStopped));
    ASSERT(spCtx->strTrace.empty());
    std::printf("⑬ 未启动执行器: 码=%d（kStepStopped）\n", r.Code());
}

// ⑭ 深链：层数超过内联深度上限时框架自动改投递（防递归爆栈）
void DemoDeepChain()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    const int kLayers = 256;  // > detail::kMaxInlineDepth（64）
    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    no::CAsyncChain<CDemoContext> tail = exec.Submit(spCtx, &StepReadParam, ASYNC_LOC);
    for (int i = 0; i < kLayers; ++i)
    {
        tail = tail.Then(&StepScale, ASYNC_LOC);
    }
    ASSERT(tail.Get().IsOk());
    ASSERT(spCtx->nScaled == 30);  // 缩放层重复执行，结果不变
    std::printf("⑭ 深链: %d 层全部执行完成\n", kLayers + 1);
    exec.Stop();
}

// ⑮ 注册点源码位置（ASYNC_LOC）：调试构建保存注册点函数 / 文件 / 行号
void DemoSourceLoc()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    no::CAsyncChain<CDemoContext> tail = exec.Submit(spCtx, &StepReadParam, ASYNC_LOC).Then(&StepScale, ASYNC_LOC);
    const no::CSourceLoc loc = tail.Loc();
    (void)loc;  // 调试构建下：loc.szFunction / szFile / nLine 指向注册点
    std::printf("⑮ 注册点源码位置: 调试构建下 tail.Loc() = %s:%d\n", loc.szFile != NULL ? loc.szFile : "(发布构建为空)",
                loc.nLine);
    ASSERT(tail.Get().IsOk());
    exec.Stop();
}

// ---------------- 协程（用顺序代码 await 多条链） ----------------

/// 协程：顺序 await 三条子链（子链与协程共享同一上下文）。
class CDemoCoroutine : public no::CCoroutine<CDemoContext>
{
   public:
    using no::CCoroutine<CDemoContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(Chain(&StepReadParam));
        CO_AWAIT(Chain(&StepScale));
        CO_AWAIT(Chain(&StepStore));
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 协程：并行 await 多条子链。
class CParallelCoroutine : public no::CCoroutine<CDemoContext>
{
   public:
    using no::CCoroutine<CDemoContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_ALL(Chain(&StepReadParam), Chain(&StepScale), Chain(&StepStore));
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 协程：await 到失败 → 终止（失败码透传，后续 await 不执行）。
class CFailCoroutine : public no::CCoroutine<CDemoContext>
{
   public:
    using no::CCoroutine<CDemoContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(Chain(&StepReadParam));
        CO_AWAIT(Chain(&StepStore));    // 失败
        CO_AWAIT(Chain(&StepCleanup));  // 不执行
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 子协程：await 一条子链。
class CChildCoroutine : public no::CCoroutine<CDemoContext>
{
   public:
    using no::CCoroutine<CDemoContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(Chain(&StepScale));
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 父协程：await 子协程（嵌套）。
class CParentCoroutine : public no::CCoroutine<CDemoContext>
{
   public:
    explicit CParentCoroutine(const std::shared_ptr<CDemoContext>& spCtx, no::CAsyncExecutor* pExec)
        : no::CCoroutine<CDemoContext>(spCtx), m_pExec(pExec), m_pChild()
    {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(Chain(&StepReadParam));
        m_pChild = m_pExec->CoStart<CChildCoroutine>(GetContext());  // 跨 await 的变量须为成员
        CO_AWAIT(m_pChild->AsChain());
        CO_RETURN_VOID();
        CO_END();
    }

   private:
    no::CAsyncExecutor* m_pExec;
    std::shared_ptr<CChildCoroutine> m_pChild;
};

// ⑯ 协程顺序 await：CO_AWAIT
void DemoCoroutineSequential()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->nBase = 7;
    std::shared_ptr<CDemoCoroutine> pCoro = exec.CoStart<CDemoCoroutine>(spCtx);
    const no::CStepResult r = pCoro->Get();
    ASSERT(r.IsOk());
    ASSERT(pCoro->GetContext() == spCtx);  // 协程与子链共享同一上下文
    ASSERT(spCtx->nScaled == 21);
    ASSERT(spCtx->strTrace == std::string("读参数;缩放;落库;"));
    std::printf("⑯ 协程顺序 await: 缩放=%d 轨迹=%s\n", spCtx->nScaled, spCtx->strTrace.c_str());
    exec.Stop();
}

// ⑰ 协程并行 await：CO_AWAIT_ALL
void DemoCoroutineParallel()
{
    no::CAsyncExecutor exec(4);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->nBase = 7;
    std::shared_ptr<CParallelCoroutine> pCoro = exec.CoStart<CParallelCoroutine>(spCtx);
    ASSERT(pCoro->Get().IsOk());
    ASSERT(spCtx->nScaled == 21);  // 数据在上下文里（顺序不定）
    std::printf("⑰ 协程并行 await: 缩放=%d 轨迹=%s\n", spCtx->nScaled, spCtx->strTrace.c_str());
    exec.Stop();
}

// ⑱ 协程 await 失败：协程终止，失败码透传，后续 await 不执行
void DemoCoroutineAwaitFailed()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->bFailStore = true;
    std::shared_ptr<CFailCoroutine> pCoro = exec.CoStart<CFailCoroutine>(spCtx);
    const no::CStepResult r = pCoro->Get();
    ASSERT(r.IsFailed());
    ASSERT(r.Code() == kCodeStoreFailed);
    ASSERT(spCtx->strTrace == std::string("读参数;落库;"));  // 清理层未执行
    std::printf("⑱ 协程 await 失败: 码=%d 轨迹=%s\n", r.Code(), spCtx->strTrace.c_str());
    exec.Stop();
}

// ⑲ 嵌套协程：await 子协程（AsChain），共用同一上下文
void DemoCoroutineNested()
{
    no::CAsyncExecutor exec(2);
    ASSERT(exec.Start());

    std::shared_ptr<CDemoContext> spCtx = std::make_shared<CDemoContext>();
    spCtx->nBase = 4;
    std::shared_ptr<CParentCoroutine> pCoro = exec.CoStart<CParentCoroutine>(spCtx, &exec);
    const no::CStepResult r = pCoro->Get();
    ASSERT(r.IsOk());
    ASSERT(spCtx->nScaled == 12);
    ASSERT(spCtx->strTrace == std::string("读参数;缩放;"));
    std::printf("⑲ 嵌套协程: 缩放=%d 轨迹=%s\n", spCtx->nScaled, spCtx->strTrace.c_str());
    exec.Stop();
}

int main()
{
    DemoSubmitAndGet();
    DemoChainThen();
    DemoUpStepToNextLayer();
    DemoFailFast();
    DemoRollbackAndRecover();
    DemoExceptionToFailed();
    DemoCompletedCallback();
    DemoFork();
    DemoTimelineAndContext();
    DemoPost();
    DemoConcurrent();
    DemoLifetime();
    DemoNotStarted();
    DemoDeepChain();
    DemoSourceLoc();
    DemoCoroutineSequential();
    DemoCoroutineParallel();
    DemoCoroutineAwaitFailed();
    DemoCoroutineNested();

    if (g_nAssertFailures == 0)
    {
        std::printf("全部断言通过 ✔\n");
        return 0;
    }
    std::printf("共 %d 个断言失败\n", g_nAssertFailures);
    return 1;
}
