#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "Async/AsyncExecutor.h"
#include "Async/AsyncTypes.h"
#include "Async/SourceLoc.h"
#include "Async/StepResult.h"

// ====================================================================
// 异步链（CAsyncChain）—— 本分支的异步特化核心
//
// 与「层间传任意值」的通用任务链相反，本框架的链只有一种类型：
//
//     层与层之间只传「本层成功 / 失败」（CStepResult），
//     数据统一放在共享上下文（std::shared_ptr<TContext>）里。
//
// 因此层函数签名固定为：
//
//     CStepResult fn(CStepResult upStep,
//                    const std::shared_ptr<TContext>& spContext);
//
//  - upStep：上一层的结果（第一层恒为成功）—— 下一层据此判断上一层回调，
//    可据此决定「透传失败」还是「吞掉失败继续」（见 ThenAlways）；
//  - spContext：整条链共享的数据载体（链持有，恒非空）；
//  - 返回：本层结果，成功继续、失败终止。
//
// 语义：
//  - 失败即停（Then）：某层返回失败后，后续 Then 层不再执行，失败码沿链透传
//    到收尾回调与 Get()（与 Promise / C# async 的失败传播一致）；
//  - 失败也执行（ThenAlways）：上一层失败时仍以失败状态调用本层，供回滚 /
//    补偿 / 清理使用；本层返回 upStep 即继续透传失败，返回成功即吞掉失败、
//    链继续往后跑；
//  - upStep 参数：上一层的结果。失败即停时本层不会被调用，能拿到失败状态的
//    只有 ThenAlways 层与 OnCompleted 收尾回调；
//  - 顺序执行：一道链的层在同一工作线程上依次执行（首层投递一次，后续层
//    级联执行，不再逐层入队）；层内如需并行，自行投递重活；
//  - 同一链段可注册多个 Then（分叉），各自独立延续；
//  - 注册时机自由：上游未完成时登记（上游完成时触发）、上游已完成时投递
//    异步触发，两者都不阻塞调用方。
//
// 用法：
// @code
// struct CLoginContext                      // 一次登录流程的共享数据。
// {
//     std::string strAccount;
//     std::string strToken;
//     std::string strError;
// };
//
// CStepResult StepReadParam(CStepResult upStep, const std::shared_ptr<CLoginContext>& spCtx)
// {
//     if (upStep.IsFailed()) { return upStep; }          // 上一层失败：透传
//     spCtx->strAccount = ReadAccountFromRequest();
//     return spCtx->strAccount.empty() ? CStepResult::Failed(kCodeNoAccount)
//                                      : CStepResult::Ok();
// }
//
// CStepResult StepVerify(CStepResult upStep, const std::shared_ptr<CLoginContext>& spCtx)
// {
//     if (upStep.IsFailed()) { return upStep; }          // 上一层（StepReadParam）失败
//     spCtx->strToken = Verify(spCtx->strAccount);
//     return CStepResult::Ok();
// }
//
// common::async::CAsyncExecutor exec(2);
// exec.Start();
//
// std::shared_ptr<CLoginContext> spCtx = std::make_shared<CLoginContext>();
// auto chain = exec.Submit(spCtx, StepReadParam, ASYNC_LOC).Then(StepVerify, ASYNC_LOC);
// chain.OnCompleted([](common::async::CStepResult finalStep) { /* 收尾 */ });
// common::async::CStepResult r = chain.Get();            // 阻塞取最终结果（成败）
// (void)r;
// @endcode
// ====================================================================

namespace common {
namespace async {

template <typename TContext>
class CAsyncChain;
template <typename TContext>
class CCoroutine;

namespace detail {

/// @brief 链段共享状态：层结果 + 续接列表 + 同步等待。
///
/// 一道链由若干「链段」串成，一段对应一层（首段对应首层）。段是单向开关：
/// 首次 Complete 生效，之前登记的续接（下一层 / 收尾回调）随后在锁外触发。
class CChainSegment
{
   public:
    /// 续接回调：接收本段最终结果。
    using Continuation = std::function<void(const CStepResult&)>;

    /// @brief 创建段（未就绪）。
    CChainSegment() : m_bReady(false), m_result() {}

    /// @brief 完成本段并触发续接（锁外调用续接，防重入死锁）。
    ///
    /// 仅首次生效；先唤醒 Wait 等待者，再按注册顺序在锁外调用所有续接。
    /// 续接在调用线程上执行 —— 链的逐层推进就在这条调用路径上级联完成。
    ///
    /// @param result 本段最终结果（成功 / 失败）。
    void Complete(const CStepResult& result)
    {
        std::vector<Continuation> vecCallbacks;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_bReady.load(std::memory_order_relaxed))
            {
                return;  // 单次生效。
            }
            m_result = result;
            m_bReady.store(true, std::memory_order_relaxed);  // 锁内写；relaxed 即可。
            vecCallbacks.swap(m_vecContinuations);
        }

        // 支持多线程等待同一段（并发 Get）：notify_all 唤醒所有等待者。
        m_cv.notify_all();

        for (size_t i = 0; i < vecCallbacks.size(); ++i)
        {
            if (vecCallbacks[i])
            {
                vecCallbacks[i](result);
            }
        }
    }

    /// @brief 注册续接；本段已完成时投递到执行器异步触发（不阻塞调用方）。
    ///
    /// @param pHandle 执行器句柄（段已完成时投递用）。
    /// @param fnContinuation 续接回调（按值接收，登记时移动存储避免拷贝）。
    /// @return true 已登记或已投递；false 已完成但执行器不可用（回调不执行）。
    bool AddContinuation(const std::shared_ptr<CExecutorHandle>& pHandle, Continuation fnContinuation)
    {
        bool bFireNow = false;
        CStepResult result;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_bReady.load(std::memory_order_relaxed))
            {
                m_vecContinuations.push_back(std::move(fnContinuation));
                return true;  // 未完成：已登记，段完成时触发。
            }
            bFireNow = true;
            result = m_result;
        }

        if (bFireNow && fnContinuation)
        {
            // 已完成：投递到执行器异步执行（与 JS / C# 一致），保持调用方不阻塞。
            std::function<void()> fnRun = [fnContinuation, result]() { fnContinuation(result); };
            if (PostToHandle(pHandle, std::move(fnRun)))
            {
                return true;
            }
            return false;  // 已完成但执行器不可用。
        }
        return true;
    }

    /// @brief 阻塞等待本段结果（先短自旋，超时再阻塞等待）。
    ///
    /// @return 本段最终结果（成功 / 失败）。
    CStepResult Wait()
    {
        // 短自旋（relaxed 读仅作宽松提示）；最终由锁内条件判定。
        const auto spinDeadline = std::chrono::steady_clock::now() + std::chrono::microseconds(50);
        while (!m_bReady.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < spinDeadline)
        {
            std::this_thread::yield();
        }

        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]() { return m_bReady.load(std::memory_order_relaxed); });
        return m_result;
    }

    /// @brief 本段是否已完成。
    bool IsCompleted() const { return m_bReady.load(std::memory_order_relaxed); }

    /// @brief 设置本段的注册点源码位置（调试用；发布构建为空操作）。
    ///
    /// @param loc 源码位置（建议传 ASYNC_LOC）。
    void SetLoc(const CSourceLoc& loc)
    {
#if defined(ASYNC_DEBUG_TRACE)
        m_loc = loc;
#else
        (void)loc;
#endif
    }

    /// @brief 获取本段注册点源码位置（发布构建恒为空）。
    CSourceLoc Loc() const
    {
#if defined(ASYNC_DEBUG_TRACE)
        return m_loc;
#else
        return CSourceLoc();
#endif
    }

   private:
    std::mutex m_mutex;                            ///< 保护结果与续接列表。
    std::condition_variable m_cv;                  ///< 通知 Wait 等待者。
    std::vector<Continuation> m_vecContinuations;  ///< 续接列表（未完成时）。
    std::atomic<bool> m_bReady;                    ///< 是否已完成（自旋读用）。
    CStepResult m_result;                          ///< 最终结果（完成后有效）。
#if defined(ASYNC_DEBUG_TRACE)
    CSourceLoc m_loc;  ///< 注册点源码位置（调试用）。
#endif
};

/// @brief 链共享核心：执行器句柄 + 共享上下文。
///
/// 一道链的所有层共用同一个核心（同一上下文 + 同一执行器），
/// 句柄持有者彼此保活（执行器析构后链仍安全跑完）。
template <typename TContext>
class CChainCore
{
   public:
    /// @brief 创建核心。
    ///
    /// @param pHandle 执行器句柄（可为空：无效链）。
    /// @param spContext 共享上下文（可为空：首次 GetContext() 时懒创建）。
    CChainCore(const std::shared_ptr<CExecutorHandle>& pHandle, const std::shared_ptr<TContext>& spContext)
        : m_pHandle(pHandle), m_spContext(spContext)
    {}

    /// @brief 共享上下文（懒创建，恒非空）。
    ///
    /// 外部传入上下文时直接返回该实例；未传入时首次调用创建（默认构造）。
    std::shared_ptr<TContext> Context() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_spContext)
        {
            m_spContext = std::make_shared<TContext>();
        }
        return m_spContext;
    }

    /// @brief 执行器句柄（投递用）。
    const std::shared_ptr<CExecutorHandle>& Handle() const { return m_pHandle; }

    /// @brief 绑定执行器句柄（协程 Start 时注入）。
    ///
    /// @param pHandle 执行器句柄。
    void SetHandle(const std::shared_ptr<CExecutorHandle>& pHandle) { m_pHandle = pHandle; }

   private:
    mutable std::mutex m_mutex;                     ///< 保护上下文懒创建。
    std::shared_ptr<CExecutorHandle> m_pHandle;     ///< 执行器句柄。
    mutable std::shared_ptr<TContext> m_spContext;  ///< 共享上下文。
};

/// @brief 级联内联深度（线程局部）：链逐层级联时最多连续内联多少层。
///
/// 超限则改为投递执行，避免超长链（数千层）在递归中爆栈。
const int kMaxInlineDepth = 64;

/// @brief 当前线程的级联内联深度。
inline int& InlineDepth()
{
    static thread_local int s_nInlineDepth = 0;
    return s_nInlineDepth;
}

/// @brief 构造「执行本层」的任务体（捕获核心与链段保活）。
///
/// @param pCore 链共享核心（上下文 + 执行器句柄）。
/// @param pSegment 本层对应的链段（执行结果写入它）。
/// @param fnStep 层函数（固定签名）。
/// @param upStep 上一层结果。
/// @return 任务体（在工作线程上执行层函数并完成链段）。
template <typename TContext>
std::function<void()> MakeStepRunner(const std::shared_ptr<CChainCore<TContext> >& pCore,
                                     const std::shared_ptr<CChainSegment>& pSegment, const StepFn<TContext>& fnStep,
                                     const CStepResult& upStep)
{
    return [pCore, pSegment, fnStep, upStep]()
    {
        CStepResult result;
        try
        {
            // 固定签名：上一层结果 + 共享上下文（层与层之间不传任意值）。
            result = fnStep(upStep, pCore->Context());
        }
        catch (...)
        {
            result = CStepResult::Failed(kStepException);  // 层内异常 → 本层失败（框架捕获）。
        }
        pSegment->Complete(result);  // 完成本段 → 触发下游续接（同线程级联）。
    };
}

/// @brief 级联执行下一层（上游刚完成，当前已在工作线程上）。
///
/// 直接执行以省去一次入队 + 唤醒；内联深度超限时改为投递（防爆栈）。
///
/// @param pCore 链共享核心。
/// @param pSegment 本层对应的链段。
/// @param fnStep 层函数。
/// @param upStep 上一层结果。
template <typename TContext>
void RunStep(const std::shared_ptr<CChainCore<TContext> >& pCore, const std::shared_ptr<CChainSegment>& pSegment,
             const StepFn<TContext>& fnStep, const CStepResult& upStep)
{
    std::function<void()> fnRun = MakeStepRunner(pCore, pSegment, fnStep, upStep);
    if (InlineDepth() < kMaxInlineDepth)
    {
        ++InlineDepth();
        fnRun();
        --InlineDepth();
        return;
    }

    if (!PostToHandle(pCore->Handle(), std::move(fnRun)))
    {
        pSegment->Complete(CStepResult::Failed(kStepStopped));  // 执行器不可用 → 本层失败。
    }
}

/// @brief 投递执行一层（首层用：必须异步，不在起链线程上执行业务代码）。
///
/// @param pCore 链共享核心。
/// @param pSegment 首层对应的链段。
/// @param fnStep 层函数。
/// @param upStep 上一层结果（首层恒为成功）。
template <typename TContext>
void PostStep(const std::shared_ptr<CChainCore<TContext> >& pCore, const std::shared_ptr<CChainSegment>& pSegment,
              const StepFn<TContext>& fnStep, const CStepResult& upStep)
{
    std::function<void()> fnRun = MakeStepRunner(pCore, pSegment, fnStep, upStep);
    if (!PostToHandle(pCore->Handle(), std::move(fnRun)))
    {
        pSegment->Complete(CStepResult::Failed(kStepStopped));  // 执行器不可用 → 首层失败。
    }
}

}  // namespace detail

/// @brief 异步链句柄（浅句柄：拷贝共享同一条链的同一段）。
///
/// 一道链 = 共享核心（上下文 + 执行器）+ 一串链段（每层一段）。
/// 本类只是「指向某一层」的句柄：Submit 起链（指向首层），Then / ThenAlways
/// 追加一层（返回指向新层的句柄），Get / OnCompleted 作用于当前指向的那一层。
///
/// 失败即停：某层失败后，其后的 Then 层不再执行（失败码透传），Get() 返回该失败。
/// 需要「失败也能执行」（回滚 / 补偿 / 清理）时用 ThenAlways。
///
/// @tparam TContext 共享上下文类型（用户自定义的流程数据结构）。
template <typename TContext>
class CAsyncChain
{
   public:
    /// 层函数类型（固定签名：上一层结果 + 共享上下文 → 本层结果）。
    using StepFn = detail::StepFn<TContext>;
    /// 完成回调类型（链跑完时触发一次，携带最终结果）。
    using CompletedFn = CCompletedFn;

    /// @brief 创建无效链（未绑定执行器；供成员声明 / 后续赋值用）。
    ///
    /// 无效链上 Submit / Then 为空操作，Get() 返回失败（kStepStopped）。
    CAsyncChain() : m_pCore(), m_pSegment() {}

    /// @brief 创建链（上下文由链在首次取用时创建）。
    ///
    /// @param executor 执行器（起链与续接投递用）。
    explicit CAsyncChain(CAsyncExecutor& executor)
        : m_pCore(std::make_shared<detail::CChainCore<TContext> >(executor.Handle(), std::shared_ptr<TContext>())),
          m_pSegment()
    {}

    /// @brief 创建链（使用外部已备好的共享上下文）。
    ///
    /// @param executor 执行器（起链与续接投递用）。
    /// @param spContext 共享上下文（外部持有；链内所有层共用该实例）。
    CAsyncChain(CAsyncExecutor& executor, const std::shared_ptr<TContext>& spContext)
        : m_pCore(std::make_shared<detail::CChainCore<TContext> >(executor.Handle(), spContext)), m_pSegment()
    {}

    /// @brief 是否有效（已绑定执行器）。
    bool IsValid() const { return m_pCore != nullptr && m_pSegment != nullptr; }

    /// @brief 起链：注册首层并立即投递执行（起点结果视为成功）。
    ///
    /// 首层在调用返回后异步执行（不在调用线程上执行层函数）。
    ///
    /// @param fnStep 首层函数（固定签名）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 指向首层的本句柄（可继续 Then 追加层）。
    CAsyncChain& Submit(const StepFn& fnStep, const CSourceLoc& loc = CSourceLoc());

    /// @brief 追加一层（失败即停）：上一层成功时执行 fnStep，上一层失败时失败透传（本层不执行）。
    ///
    /// 上游未完成时登记（上游完成时在本线程级联执行）；上游已完成时投递到
    /// 执行器异步触发。同一段多次 Then 即分叉，各自独立延续。
    ///
    /// @param fnStep 本层函数（固定签名）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 指向本层的链句柄（后续 Get / Then / OnCompleted 作用于本层）。
    CAsyncChain Then(const StepFn& fnStep, const CSourceLoc& loc = CSourceLoc()) const;

    /// @brief 追加一层（失败也执行）：上一层无论成败都会调用 fnStep，upStep 即上一层结果。
    ///
    /// 用于回滚 / 补偿 / 清理等「无论成败都要跑」的层：
    ///  - 返回 upStep：继续透传失败（失败时后续 Then 层仍不执行）；
    ///  - 返回成功：吞掉失败，链从本层之后继续（后续 Then 层重新执行）。
    ///
    /// @param fnStep 本层函数（固定签名，须自行判断 upStep.IsFailed()）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 指向本层的链句柄。
    CAsyncChain ThenAlways(const StepFn& fnStep, const CSourceLoc& loc = CSourceLoc()) const;

    /// @brief 注册完成回调（链跑完时触发一次，成功 / 失败都触发）。
    ///
    /// @param fnCompleted 完成回调（入参为最终层结果）。
    /// @return true 已登记或已投递；false 链已完成但执行器不可用（回调不执行）。
    bool OnCompleted(const CompletedFn& fnCompleted) const;

    /// @brief 阻塞等待当前层结果（不抛异常）。
    ///
    /// @return 当前层最终结果；无效链返回失败（kStepStopped）。
    CStepResult Get() const;

    /// @brief 共享上下文（懒创建，有效链上恒非空）。
    ///
    /// 外部可先取上下文填初始数据，再 Submit 起链；也可在链内任意层读写。
    std::shared_ptr<TContext> GetContext() const;

    /// @brief 当前层是否已完成。
    bool IsCompleted() const { return m_pSegment != nullptr && m_pSegment->IsCompleted(); }

    /// @brief 当前层的注册点源码位置（调试用；发布构建恒为空）。
    CSourceLoc Loc() const { return m_pSegment != nullptr ? m_pSegment->Loc() : CSourceLoc(); }

   private:
    /// @brief 内部：追加一层（bAlways 控制失败时是否仍执行）。
    ///
    /// @param fnStep 本层函数。
    /// @param loc 注册点源码位置。
    /// @param bAlways true 失败也执行（ThenAlways）；false 失败即停（Then）。
    /// @return 指向本层的链句柄。
    CAsyncChain AddStep(const StepFn& fnStep, const CSourceLoc& loc, bool bAlways) const;

    /// @brief 内部：从共享核心与链段构造句柄（协程 AsChain 用）。
    ///
    /// @param pCore 链共享核心。
    /// @param pSegment 链段（本句柄指向的层）。
    /// @return 指向该段的链句柄。
    static CAsyncChain Make(const std::shared_ptr<detail::CChainCore<TContext> >& pCore,
                            const std::shared_ptr<detail::CChainSegment>& pSegment)
    {
        CAsyncChain chain;
        chain.m_pCore = pCore;
        chain.m_pSegment = pSegment;
        return chain;
    }

    friend class CAsyncExecutor;        // Submit 起链。
    friend class CCoroutine<TContext>;  // 协程 AsChain / 子链。

    std::shared_ptr<detail::CChainCore<TContext> > m_pCore;  ///< 共享核心（上下文 + 执行器）。
    std::shared_ptr<detail::CChainSegment> m_pSegment;       ///< 当前层对应的链段。
};

// ===========================================================================
// ============================== 链成员实现 =================================
// ===========================================================================

/// @brief 起链：注册首层并立即投递执行。
///
/// 首层必须异步执行（不在起链线程上跑业务代码），因此固定走投递路径；
/// 首层执行结果完成首段，其后续接（下一层 / 完成回调）随之被触发。
///
/// @param fnStep 首层函数（固定签名）。
/// @param loc 注册点源码位置（可选）。
/// @return 本句柄（此时指向首层）。
template <typename TContext>
CAsyncChain<TContext>& CAsyncChain<TContext>::Submit(const StepFn& fnStep, const CSourceLoc& loc /* = CSourceLoc() */)
{
    if (m_pCore == nullptr)
    {
        return *this;  // 无效链：空操作。
    }

    m_pSegment = std::make_shared<detail::CChainSegment>();
    m_pSegment->SetLoc(loc);

    // 起点结果视为成功：首层据 upStep.IsOk() 判断「尚无上一层」，业务上可忽略。
    detail::PostStep(m_pCore, m_pSegment, fnStep, CStepResult::Ok());
    return *this;
}

/// @brief 追加一层（失败即停）：上一层成功时执行，上一层失败时失败透传。
///
/// 注册方式二选一（都不阻塞调用方）：
///  - 上游未完成：登记续接，上游完成时由该线程级联执行本层；
///  - 上游已完成：投递到执行器异步执行本层（与 JS / C# 一致）。
///
/// @param fnStep 本层函数（固定签名）。
/// @param loc 注册点源码位置（可选）。
/// @return 指向本层的链句柄。
template <typename TContext>
CAsyncChain<TContext> CAsyncChain<TContext>::Then(const StepFn& fnStep,
                                                  const CSourceLoc& loc /* = CSourceLoc() */) const
{
    return AddStep(fnStep, loc, false);
}

/// @brief 追加一层（失败也执行）：上一层无论成败都会调用 fnStep。
///
/// @param fnStep 本层函数（固定签名，须自行判断 upStep.IsFailed()）。
/// @param loc 注册点源码位置（可选）。
/// @return 指向本层的链句柄。
template <typename TContext>
CAsyncChain<TContext> CAsyncChain<TContext>::ThenAlways(const StepFn& fnStep,
                                                        const CSourceLoc& loc /* = CSourceLoc() */) const
{
    return AddStep(fnStep, loc, true);
}

/// @brief 追加一层的实现（Then / ThenAlways 共用）。
///
/// 注册方式二选一（都不阻塞调用方）：
///  - 上游未完成：登记续接，上游完成时由该线程级联执行本层；
///  - 上游已完成：投递到执行器异步执行本层（与 JS / C# 一致）。
///
/// @param fnStep 本层函数（固定签名）。
/// @param loc 注册点源码位置（可选）。
/// @param bAlways true 失败也执行（ThenAlways）；false 失败即停（Then）。
/// @return 指向本层的链句柄。
template <typename TContext>
CAsyncChain<TContext> CAsyncChain<TContext>::AddStep(const StepFn& fnStep, const CSourceLoc& loc, bool bAlways) const
{
    CAsyncChain<TContext> chainNext;
    if (m_pCore == nullptr || m_pSegment == nullptr)
    {
        return chainNext;  // 无效链：返回无效句柄（不注册任何续接）。
    }

    chainNext.m_pCore = m_pCore;  // 与上游共享核心：同一上下文 + 同一执行器。
    chainNext.m_pSegment = std::make_shared<detail::CChainSegment>();
    const std::shared_ptr<detail::CChainSegment> pNextSegment = chainNext.m_pSegment;
    pNextSegment->SetLoc(loc);

    const std::shared_ptr<detail::CChainCore<TContext> > pCore = m_pCore;
    const std::shared_ptr<detail::CChainSegment> pUpSegment = m_pSegment;

    const bool bOk =
        pUpSegment->AddContinuation(pCore->Handle(), [pCore, pNextSegment, fnStep, bAlways](const CStepResult& upStep)
    {
        // ① 失败即停（Then）：本层不执行，失败码原样交给下游（继续透传）。
        if (upStep.IsFailed() && !bAlways)
        {
            pNextSegment->Complete(upStep);
            return;
        }

        // ② 执行本层：upStep 携带上一层结果（ThenAlways 层可能是失败状态）。
        detail::RunStep(pCore, pNextSegment, fnStep, upStep);
    });

    if (!bOk)
    {
        // 上游已完成但执行器不可用：本层无法执行，以失败结束（下游继续透传）。
        pNextSegment->Complete(CStepResult::Failed(kStepStopped));
    }
    return chainNext;
}

/// @brief 注册完成回调（链跑完时触发一次，成功 / 失败都触发）。
///
/// @param fnCompleted 完成回调（入参为最终层结果）。
/// @return true 已登记或已投递；false 链已完成但执行器不可用（回调不执行）。
template <typename TContext>
bool CAsyncChain<TContext>::OnCompleted(const CompletedFn& fnCompleted) const
{
    if (m_pCore == nullptr || m_pSegment == nullptr)
    {
        return false;  // 无效链：无法注册。
    }

    return m_pSegment->AddContinuation(m_pCore->Handle(), [fnCompleted](const CStepResult& finalStep)
    {
        if (fnCompleted)
        {
            fnCompleted(finalStep);
        }
    });
}

/// @brief 阻塞等待当前层结果（不抛异常）。
///
/// @return 当前层最终结果；无效链返回失败（kStepStopped）。
template <typename TContext>
CStepResult CAsyncChain<TContext>::Get() const
{
    if (m_pSegment == nullptr)
    {
        return CStepResult::Failed(kStepStopped);  // 无效链：无结果可等。
    }
    return m_pSegment->Wait();
}

/// @brief 共享上下文（懒创建，有效链上恒非空）。
///
/// @return 共享上下文；无效链返回空 shared_ptr。
template <typename TContext>
std::shared_ptr<TContext> CAsyncChain<TContext>::GetContext() const
{
    if (m_pCore == nullptr)
    {
        return std::shared_ptr<TContext>();
    }
    return m_pCore->Context();
}

/// @brief 起链实现（执行器入口）：创建链并投递首层。
///
/// @tparam TContext 上下文类型（由 spContext 推导）。
/// @param spContext 链的共享上下文。
/// @param fnStep 首层函数。
/// @param loc 注册点源码位置（可选）。
/// @return 指向首层的链句柄。
template <typename TContext>
CAsyncChain<TContext> CAsyncExecutor::Submit(const std::shared_ptr<TContext>& spContext,
                                             typename CAsyncChain<TContext>::StepFn fnStep,
                                             const CSourceLoc& loc /* = CSourceLoc() */)
{
    CAsyncChain<TContext> chain(*this, spContext);
    chain.Submit(fnStep, loc);
    return chain;
}

}  // namespace async
}  // namespace common
