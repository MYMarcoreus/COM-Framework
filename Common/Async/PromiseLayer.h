#pragma once

#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>

#include "Assert.h"
#include "Async/AsyncExecutor.h"
#include "Async/GateGuard.h"
#include "Async/PromiseResult.h"
#include "Async/PromiseState.h"
#include "Async/PromiseTypes.h"
#include "Async/SourceLoc.h"
#include "Async/Trace.h"

// ====================================================================
// 层运行器（`MakeLayerRunner` / `MakeThenRunner` / `MakeResultRunner`）
//
// 从 Promise.h 拆出（2026-09-26）：这一块是「怎么把一层跑起来」—— 框架里**唯一**跑用户处理器
// 的地方，也是 `ASYNC_GATE`（层体自请过门）挂起重入的落点。与 promise 句柄无关，只依赖：
// 层状态（PromiseState.h）、执行器（AsyncExecutor.h）、门（GateGuard.h）、调试帧（Trace.h）。
//
// 读这一块时只要记住三件事：
//   ① 压层体调用作用域（`detail::CGateCallScope`）→ 跑处理器（异常也收成本层结果）→ 见挂起
//      标记就**不 settle**，改为把「再跑一遍本层」按请求类别过门投递；
//   ② then / catch / finally 与首层共用 `MakeLayerRunner` 这层外壳，差别只在 `fnBody` 怎么调
//      用户处理器（`MakeThenRunner` 只给上下文、`MakeResultRunner` 给「上游结果 + 模式」）；
//   ③ 三态语义（谁跳过 / 谁透传）在**追加层**那一步判定（见 Promise.h 的 `Append*Layer`），
//      这里只负责「跑 + 收口」。
// ====================================================================

namespace common {
namespace async {

namespace detail {

// ================= 三态语义（JS 的 then / catch / finally）：三条规则，各写在各处 =================
//
// 规则只有三条，各写在它该在的地方 —— 「没有公共的「按模式分派」函数」：模式在各自的调用点
// 就是常量，多一层函数只是多一次跳转。
//
//   - then    ：上游兑现才执行；`AppendThenLayer` 判 `IsRejected()` 决定跳过；本层结果 = 处理器返回值
//               （`MakeThenRunner`，它拿不到上游结果）。
//   - catch   ：上游被拒绝才执行；`AppendResultLayer` 判 `kModeCatch && IsFulfilled()` 决定跳过；
//               本层结果 = 处理器返回值（返回 `Resolve()` 即恢复链）。
//   - finally ：兑现 / 拒绝都执行，「没有跳过分支」（所以那个判断里不该出现 finally）；本层结果 =
//               忽略处理器返回值、原样透传 upResult（`MakeResultRunner`）。
//
// 最后一条是最容易看漏的：finally 「既不跳过、也不改结果」，所以它的 eMode 只用在「透传上一层
// 结果」那一步，「不参与任何跳过判断」 —— 硬写成通用条件的话（`(then && 被拒) || (catch && 已兑现)`），
// 它在 finally 上恒为 false，等于每次白判一次。

/// @brief 造「执行本层处理器」的任务体 —— 「框架里唯一跑用户处理器的地方」。
///
/// then / catch / finally 与首层共用这一层外壳，差别只在 `fnBody` 怎么调用户处理器。
/// 任务体只捕获「它真正需要的东西」（`fnBody` 自己带着上下文与处理器），
/// 这样在途任务不需要靠核心存活（因此核心无需 `enable_shared_from_this`），
/// 也让「保活链」短一截：任务跑完前，只有它自己用到的对象在。
///
/// 另一件事：压「层体调用作用域」（`detail::CGateCallScope`）—— 层体里的 `ASYNC_GATE`
/// 靠它判定「我是不是已经在目标槽位里」；不在时层体返回占位结果，这里把「重跑一遍本层」
/// 按请求的类别过门投递（本层**不 settle**，所以下游不会提前跑）。
///
/// @param pHandle 本链执行器句柄（重入投递用；恒非空）。
/// @param pState 本层状态（执行结果写入它）。
/// @param fnBody 执行体（返回本层结果）。
/// @param bRetried 本层这次运行是否已因「自请过门」挂起过一次（重入运行器传 true；判据见 GateGuard.h：
///                 一次运行只允许挂起一次，第二次挂起即收口）。
/// @return 任务体（在工作线程上执行处理器并 settle 本层状态）。
template <typename TBody>
std::function<void()> MakeLayerRunner(const std::shared_ptr<CExecutorHandle>& pHandle,
    const std::shared_ptr<CPromiseState>& pState, TBody fnBody, bool bRetried = false)
{
    return [pHandle, pState, fnBody, bRetried]()
    {
        // ① 压层体调用作用域（TLS，零分配）：`ASYNC_GATE` 的判定与挂起请求都写在这里。
        const CGateCallScope scope(pHandle->m_pGate.get());
#if defined(ASYNC_DEBUG_TRACE)
        // ①′ 压 trace 帧：处理器内部就能通过 Trace.h 看到自己处在哪条链上。
        //    帧活在本次调用的栈上（零分配）；内联级联会自然形成嵌套的帧栈，每层自己弹自己。
        const CCurrentLayerFrame frame(pState);
#endif
        // ② 跑处理器：无论怎么结束（正常返回 / 抛异常）都要得到一份本层结果。
        CPromiseResult result;
        try
        {
            result = fnBody();
        }
        catch (const std::exception& e)
        {
            // 处理器抛异常 → 本层以该异常的文本收口（finally 抛异常同样覆盖）；
            // 类型降级为 std::runtime_error（结果里存的是自有的共享异常对象，装不下「在飞的异常」）。
            result = CPromiseResult::Reject(std::runtime_error(e.what()));
        }
        catch (...)
        {
            result = CPromiseResult::Reject(std::runtime_error("处理器异常"));  // 非 std 异常：只留一句说明
        }

        // ③ 层体自请过门（`ASYNC_GATE`）：本次只跑到请求那一行 —— 上面的 result 是占位值。
        //    不 settle，改为把「再跑一遍本层」按请求类别过门投递；重入时帧已匹配，请求那一行落穿。
        if (scope.bSuspended)
        {
            // 兜底：一次运行只允许挂起一次（编译期已保证「一个层体只声明一次」，重入后必然落穿）。
            // 第二次挂起 = 漏网的换档写法（辅助函数里再声明门 / 绕过宏直接请求）→ 收成**有界失败**
            //（诊断 + 本层收口），而不是让链在门之间来回换档。
            if (bRetried)
            {
                ReportDiagnostic(kDiagGateSuspendedTwice);
                pState->Settle(CPromiseResult::Reject(std::runtime_error("ASYNC_GATE 重复挂起")));
                return;
            }
            std::function<void()> fnRetry = MakeLayerRunner(pHandle, pState, fnBody, /* bRetried = */ true);
            if (!PostToHandle(pHandle, scope.eRequested, std::move(fnRetry)))
            {
                // 执行器不可用 → 本层以「执行器已停」收口（链绝不永久 pending）。
                pState->Settle(CPromiseResult::Reject(std::runtime_error("执行器已停")));
            }
            return;
        }

#if defined(ASYNC_DEBUG_TRACE)
        // ④ 记本层耗时（必须在 settle 前写：落定后这层就可能被别的线程读了）。
        pState->SetSelfDurationMs(frame.ElapsedMs());
#endif
        // ⑤ 落定本层 → 触发下一层（同执行器就地级联 / 跨执行器投递）。
        pState->Settle(result);
    };
}

/// @brief 造 then 层（含首层）的任务体：处理器「看不到上游结果」，直接返回本层结果。
///
/// @param pHandle 本链执行器句柄（层体自请过门要用）。
/// @param spContext 共享上下文（调用方在构造任务时解析好，恒非空）。
/// @param pState 本层状态（执行结果写入它）。
/// @param fnHandler 处理器（then 签名）。
/// @return 任务体。
template <typename TContext>
std::function<void()> MakeThenRunner(const std::shared_ptr<CExecutorHandle>& pHandle, const std::shared_ptr<TContext>& spContext,
    const std::shared_ptr<CPromiseState>& pState, const ThenHandler<TContext>& fnHandler)
{
    ASSERT(spContext != nullptr);  // 任务体把上下文按值捕获交给处理器：必须已经备好。

    // 执行体只做一件事：把共享上下文交给处理器（then 拿不到上游结果）；
    // 帧 / 异常收口 / settle / 自请过门都是外壳（MakeLayerRunner）的事。
    return MakeLayerRunner(pHandle, pState,
        [spContext, fnHandler]()
        {
            return fnHandler(spContext);
        });
}

/// @brief 造 catch / finally 层的任务体：把「上游结果」交给处理器，返回值按模式归一。
///
/// @param pHandle 本链执行器句柄（层体自请过门要用）。
/// @param spContext 共享上下文（调用方在构造任务时解析好，恒非空）。
/// @param pState 本层状态（执行结果写入它）。
/// @param fnHandler 处理器（catch / finally 签名）。
/// @param upResult 上一层结果。
/// @param eMode 处理器模式（catch / finally）—— 只用来决定「结果归一」：
///              catch 取处理器返回值（返回 `Resolve()` 即恢复）；finally 忽略它、原样透传上一层结果。
/// @return 任务体。
template <typename TContext>
std::function<void()> MakeResultRunner(const std::shared_ptr<CExecutorHandle>& pHandle,
    const std::shared_ptr<TContext>& spContext, const std::shared_ptr<CPromiseState>& pState,
    const ResultHandler<TContext>& fnHandler, const CPromiseResult& upResult, HandlerMode eMode)
{
    ASSERT(spContext != nullptr);  // 任务体把上下文按值捕获交给处理器：必须已经备好。

    // 执行体两步：① 把「上游结果 + 上下文」交给处理器；② 按模式归一结果
    //（catch 取处理器返回值；finally 忽略它，原样透传上一层结果）。
    return MakeLayerRunner(pHandle, pState,
        [spContext, fnHandler, upResult, eMode]()
        {
            const CPromiseResult ownResult = fnHandler(upResult, spContext);
            return (eMode == kModeFinally) ? upResult : ownResult;
        });
}

}  // namespace detail
}  // namespace async
}  // namespace common
