#pragma once

// ====================================================================
// 组合器：把 N 个子 promise 汇成一条「聚合链」
//
// 四个入口（`exec.WhenAll` / `WhenAllSettled` / `WhenRace` / `WhenAny`）对齐 JS 的
// `Promise.all` / `allSettled` / `race` / `any`：新造一条「由子 promise 的落定驱动」的聚合链，
// 与 `exec.NewPromise` 同族的起链入口。它们是「执行器上」的入口，不是 `CPromise` 的成员
// （完整语义文档见每个入口定义）。
//
// 为什么单独成文件（2026-09-26 从 AsyncExecutor.h 拆出）：
//   组合器要造 `CPromise` 实例，因此需要 `CPromise` 的**完整类型**；而 `AsyncExecutor.h` 只能
//   前置声明 `CPromise`（`Promise.h` 反过来要 include 执行器头，不能形成环）。留在执行器头里时，
//   这条依赖靠「模板两段查找 + 调用方 TU 碰巧已 include Promise.h」兜住 —— 是个隐式契约。
//   搬来本文件后：执行器头只留 4 个**成员声明**，定义在这里显式 include `Promise.h`，
//   依赖关系明面化；用组合器的 TU include 本头即可。
//
// 四个入口共有的语义要点：
//   - 参数可为单个子 promise（`CPromise<任意上下文>`）或 `std::vector<CPromise<同上下文>>`，
//     两者可混用（按参数顺序登记）；
//   - 聚合层是**框架簿记层**（只被 settle，不跑业务代码、不碰模块状态）→ 固定 `kKindBookkeeping`
//     （= `kDirect`，不过门）；子链与聚合链的后续层各自带自己的类别；
//   - 聚合状态是纯状态（不碰上下文类型）→ 跨模块 / 跨上下文类型的分支能汇到同一个聚合上；
//   - 子 promise 的落定可能发生在**任意线程**上 → 「锁内判定、锁外收口」；
//   - 框架不提供取消：fail-fast / race 收口后，其余子 promise 照旧跑完（结果被忽略）。
// ====================================================================

#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"

namespace common {
namespace async {

namespace detail {

/// @brief 组合器策略（`WhenAll` / `WhenAllSettled` / `WhenRace` / `WhenAny` 四档）。
enum GatherPolicy
{
    kGatherAll = 0,         ///< 对齐 JS `Promise.all`：全部兑现才兑现；任一拒绝立即以该拒绝码拒绝。
    kGatherAllSettled = 1,  ///< 对齐 JS `Promise.allSettled`：全部落定即兑现（不看各分支成败）。
    kGatherRace = 2,        ///< 对齐 JS `Promise.race`：首个落定者定结果（兑现 / 拒绝皆可）。
    kGatherAny = 3          ///< 对齐 JS `Promise.any`：首个兑现者兑现；全部拒绝才以首个拒绝码拒绝。
};

/// @brief 一处子 promise 都没有时的收口结果（对齐 JS）。
///
/// `all` / `allSettled` 视为成功（没有要等的东西）；`race` / `any` 不可能有结果 →
/// 立即以框架侧拒绝「未指定原因」收口（否则聚合链永久 pending，`Await()` 会死等）。
///
/// @param ePolicy 策略（GatherPolicy 四档）。
/// @return 空集合应立即采用的最终结果。
inline CPromiseResult ResolveEmptyGather(GatherPolicy ePolicy)
{
    return (ePolicy == kGatherAll || ePolicy == kGatherAllSettled) ? CPromiseResult::Resolve()
                                                                   : CPromiseResult::Reject(std::runtime_error("未指定原因"));
}

/// @brief 组合器聚合状态（把 N 个子 promise 的落定折算成「一条聚合链」的落定）。
///
/// 与 `CPromiseState` 一样是「非模板」的纯状态：它只关心子 promise 的成败，
/// 完全不碰上下文类型 —— 所以「跨模块（不同 TContext）的分支汇到同一个聚合」不需要额外机制。
///
/// 收口动作由调用方以 resolve / reject 传入（聚合链的层状态由它们 settle）。
class CGatherState
{
public:
    /// @brief 创建聚合状态。
    ///
    /// @param ePolicy 策略（GatherPolicy 四档）。
    /// @param nTotal 子 promise 总数（> 0；空集合由调用方在收口前先处理）。
    /// @param fnResolve 兑现聚合链的当前层。
    /// @param fnReject 拒绝聚合链的当前层。
    CGatherState(GatherPolicy ePolicy, int nTotal, const std::function<void()>& fnResolve,
        const std::function<void(CPromiseResult)>& fnReject)
        : m_ePolicy(ePolicy),
          m_nPending(nTotal),
          m_bRejectSeen(false),
          m_bDone(false),
          m_firstReject(CPromiseResult::Reject(std::runtime_error("未指定原因"))),
          m_fnResolve(fnResolve),
          m_fnReject(fnReject)
    {}

    /// @brief 一个子 promise 落定（可能被不同线程并发调用）。
    ///
    /// @param result 子 promise 的最终结果。
    void OnChildSettled(const CPromiseResult& result)
    {
        bool bResolve = false;
        bool bReject = false;
        CPromiseResult resultOut;  // 收口交给聚合层的结果（锁内填、锁外交付）。
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_bDone)
            {
                return;  // 已收口：迟到的子 promise 直接忽略（框架不取消它们）。
            }

            --m_nPending;
            if (!result.IsFulfilled() && !m_bRejectSeen)
            {
                // 整份结果（码 / 文案 / 系统侧失败都不丢）——`any` 全部拒绝时用它收口。
                m_bRejectSeen = true;
                m_firstReject = result;
            }

            switch (m_ePolicy)
            {
                case kGatherAll:
                    // 任一拒绝 → 立即收口（及时失败）；全部兑现 → 才兑现。
                    if (result.IsFulfilled())
                    {
                        m_bDone = (m_nPending == 0);
                        bResolve = m_bDone;
                    }
                    else
                    {
                        m_bDone = true;
                        bReject = true;
                        resultOut = result;
                    }
                    break;
                case kGatherAllSettled:
                    // 成败都算数：全部落定即兑现（各分支的成败由调用方从子句柄读）。
                    m_bDone = (m_nPending == 0);
                    bResolve = m_bDone;
                    break;
                case kGatherRace:
                    // 首个落定者定结果（兑现 / 拒绝都算）。
                    m_bDone = true;
                    bResolve = result.IsFulfilled();
                    bReject = !bResolve;
                    resultOut = result;
                    break;
                case kGatherAny:
                default:
                    // 首个兑现者定结果；全部拒绝才拒绝（对齐 JS Promise.any）。
                    if (result.IsFulfilled())
                    {
                        m_bDone = true;
                        bResolve = true;
                    }
                    else if (m_nPending == 0)
                    {
                        m_bDone = true;
                        bReject = true;
                        resultOut = m_firstReject;
                    }
                    break;
            }
        }

        // 锁外收口：settle 聚合层会触发它的下一层（可能就地执行，持锁调用有死锁风险）。
        if (bResolve && m_fnResolve)
        {
            m_fnResolve();
        }
        if (bReject && m_fnReject)
        {
            m_fnReject(resultOut);
        }
    }

private:
    std::mutex m_mutex;                              ///< 保护下面的计数（子 promise 在不同线程上落定）。
    GatherPolicy m_ePolicy;                          ///< 策略（GatherPolicy 四档）。
    int m_nPending;                                  ///< 尚未落定的子 promise 数。
    bool m_bRejectSeen;                              ///< 是否已见过拒绝（`any` 收口要用首个拒绝结果）。
    bool m_bDone;                                    ///< 聚合是否已收口（收口后忽略迟到的子 promise）。
    CPromiseResult m_firstReject;                    ///< 首个拒绝的整份结果（m_bRejectSeen 为 true 时有效）。
    std::function<void()> m_fnResolve;               ///< 兑现聚合链的当前层。
    std::function<void(CPromiseResult)> m_fnReject;  ///< 拒绝聚合链的当前层。
};

/// @brief 把「子 promise 落定 → 聚合状态」登记到子 promise 上（组合器唯一的登记路径）。
///
/// 只登记回调、不阻塞任何线程（子 promise 已落定时由 `OnSettled` 的送达保证立即触发）。
///
/// @param pGather 聚合状态。
/// @param promiseChild 子 promise（上下文类型任意；恒有效）。
template <typename TChildContext>
void BindChildGather(const std::shared_ptr<CGatherState>& pGather, const CPromise<TChildContext>& promiseChild)
{
    // 通知恒送达（没有返回值）→ 子链落定即计入聚合。
    promiseChild.OnSettled(
        [pGather](CPromiseResult childResult)
        {
            pGather->OnChildSettled(childResult);
        });
}

/// @brief 把一个子 promise 追加到登记动作列表（`Gather` 摊平参数包用）。
///
/// C++11 的 lambda 捕获列表不能展开参数包，所以先给每个子 promise 生成一个登记动作
/// （`std::vector` 收集），再由聚合链的 executor 逐个执行。
///
/// @param vecOut 登记动作列表（追加到末尾）。
/// @param promiseChild 子 promise。
template <typename TChildContext>
void AppendGatherBindings(
    std::vector<std::function<void(const std::shared_ptr<CGatherState>&)> >& vecOut, const CPromise<TChildContext>& promiseChild)
{
    vecOut.push_back(
        [promiseChild](const std::shared_ptr<CGatherState>& pGather)
        {
            BindChildGather(pGather, promiseChild);
        });
}

/// @brief 把一组子 promise 追加到登记动作列表（数量运行时确定时用）。
///
/// 与标量版同名重载，所以 `exec.WhenAll(spCtx, pA, vecBranches, pB)` 这种「标量 + 列表混用」
/// 也能直接写。
///
/// @param vecOut 登记动作列表（追加到末尾）。
/// @param vecChild 子 promise 列表（同一上下文类型）。
template <typename TChildContext>
void AppendGatherBindings(std::vector<std::function<void(const std::shared_ptr<CGatherState>&)> >& vecOut,
    const std::vector<CPromise<TChildContext> >& vecChild)
{
    for (size_t i = 0; i < vecChild.size(); ++i)
    {
        AppendGatherBindings(vecOut, vecChild[i]);
    }
}

/// @brief 组合器的统一实现（四个 `When*` 只差一个策略）。
///
/// 参数可为单个子 promise（`CPromise<任意上下文>`），也可为 `std::vector<CPromise<同上下文>>`
/// （数量运行时确定时用），两者可混用 —— 展开后按参数顺序登记。
///
/// 聚合链的当前层用 `exec.NewPromise(spCtx, fnStarter, 簿记类别)` 造（由外部 settle）：起链回调里
/// 只做「逐个登记子 promise」，不做重活、不阻塞 —— 子 promise 落在哪个线程都不会占住聚合链的线程。
///
/// 聚合层**没有类别可给**（它是框架簿记层）：固定用 `kKindBookkeeping`（不过门）——
/// 它不跑业务代码、不碰模块状态，占一个读 / 写槽位只会无意义地阻塞模块里的真任务。
///
/// 一处子 promise 都没有时直接在此收口（对齐 JS）：`all` / `allSettled` 立即兑现；
/// `race` / `any` 不可能有结果 → 立即以框架侧拒绝「未指定原因」收口（否则永久 pending，死等）。
///
/// @note 本函数在 "Async/Combine.h" 里定义（组合器需要 `CPromise` 完整类型，见文件头说明）。
///
/// @tparam TContext 聚合 promise 的上下文类型。
/// @tparam TChild 子 promise 类型 / 子 promise 列表类型。
/// @param executor 聚合链的执行器。
/// @param spContext 聚合 promise 的共享上下文。
/// @param ePolicy 策略（GatherPolicy 四档）。
/// @param child 子 promise，或 `std::vector<CPromise<同上下文>>`。
/// @return 聚合 promise 句柄（pending；由子 promise 的落定驱动）。
template <typename TContext, typename... TChild>
CPromise<TContext> Gather(
    CAsyncExecutor& executor, const std::shared_ptr<TContext>& spContext, GatherPolicy ePolicy, const TChild&... child)
{
    std::vector<std::function<void(const std::shared_ptr<CGatherState>&)> > vecBindings;
    const int nUnused[] = {0, (AppendGatherBindings(vecBindings, child), 0)...};
    (void)nUnused;

    if (vecBindings.empty())
    {
        // 一处子 promise 都没有：按策略直接收口（语义只有 `ResolveEmptyGather` 一处）。
        // 用「一层 handler」而不是起链回调：handler 直接返回整份结果，语义最直白。
        return executor.NewPromise(spContext,
            typename CPromise<TContext>::ThenHandler(
                [ePolicy](const std::shared_ptr<TContext>& /*spContext*/)
                {
                    return ResolveEmptyGather(ePolicy);
                }),
            kKindBookkeeping);
    }

    const int nTotal = static_cast<int>(vecBindings.size());
    return executor.NewPromise(spContext,
        typename CPromise<TContext>::ChainStarter(
            [ePolicy, nTotal, vecBindings](
                const std::function<void()>& fnResolve, const std::function<void(CPromiseResult)>& fnReject)
            {
                const std::shared_ptr<CGatherState> pGather =
                    std::make_shared<CGatherState>(ePolicy, nTotal, fnResolve, fnReject);
                for (size_t i = 0; i < vecBindings.size(); ++i)
                {
                    vecBindings[i](pGather);  // 登记动作恒非空。
                }
            }),
        kKindBookkeeping);
}

}  // namespace detail

/// @brief 组合器（对齐 JS `Promise.all`）：等一组子 promise 「全部兑现」；
///        任一拒绝 → 立即以该拒绝码拒绝（其余分支继续跑完，结果被忽略）。
///
/// 用途：并行分支 / 并行调用多个模块（子 promise 「可跨上下文类型」），全部完成后继续本链。
/// 聚合 promise 只关心分支成败，「不传值」：数据请让各分支写进自己的共享上下文
/// （同上下文时共用一个实例即可）。
///
/// 语义：
///  - 全部兑现 → 聚合兑现；
///  - 「任一拒绝 → 立即以该拒绝码拒绝」（对齐 JS：及时失败；其余分支继续跑完，结果被忽略）；
///  - 已落定的子 promise 直接计入；
///  - 一处子 promise 都没给 → 立即兑现。
///
/// @note 本执行器只用于「聚合 promise 自己的层」（`.Then(...)` 等）；各子 promise 仍跑在
///       它们各自的执行器上。组合器只有这四个执行器入口（没有 `CPromise` 成员形态）。
///
/// @tparam TContext 聚合 promise 的上下文类型（由 spContext 推导）。
/// @tparam TChild 子 promise 类型 / 子 promise 列表类型。
/// @param spContext 聚合 promise 的共享上下文（「必传」；与其他起链入口一致，框架不代建）。
/// @param child 子 promise，或 `std::vector<CPromise<同上下文>>`（数量运行时确定）；两者可混用。
/// @return 聚合 promise 句柄（pending；由子 promise 的落定驱动）。
template <typename TContext, typename... TChild>
CPromise<TContext> CAsyncExecutor::WhenAll(const std::shared_ptr<TContext>& spContext, const TChild&... child)
{
    return detail::Gather(*this, spContext, detail::kGatherAll, child...);
}

/// @brief 组合器（对齐 JS `Promise.allSettled`）：等一组子 promise 「全部落定」后兑现（恒兑现）。
///
/// 与 `WhenAll` 的差别：「不因任何分支被拒绝而失败」 —— 「并行发起 N 件事，全部有结论后再继续」
/// 用它（典型：批量通知 / 收尾清理 / 并行上报，个别失败不影响整体）。
///
/// 各分支的成败在本框架里没有值通道，调用方自己读：此时各子句柄都已落定，
/// `child.Await()` 会立即返回该分支的 `CPromiseResult`（不阻塞），或事先挂 `OnSettled`。
///
/// @note 其余语义（跨上下文类型、空集合立即兑现）同 `WhenAll`。
///
/// @tparam TContext 聚合 promise 的上下文类型（由 spContext 推导）。
/// @tparam TChild 子 promise 类型 / 子 promise 列表类型。
/// @param spContext 聚合 promise 的共享上下文（「必传」）。
/// @param child 子 promise，或 `std::vector<CPromise<同上下文>>`（数量运行时确定）；两者可混用。
/// @return 聚合 promise 句柄（恒兑现；由子 promise 的落定驱动）。
template <typename TContext, typename... TChild>
CPromise<TContext> CAsyncExecutor::WhenAllSettled(const std::shared_ptr<TContext>& spContext, const TChild&... child)
{
    return detail::Gather(*this, spContext, detail::kGatherAllSettled, child...);
}

/// @brief 组合器（对齐 JS `Promise.race`）：「首个落定」的子 promise 定结果（兑现 / 拒绝皆可）。
///
/// 用法：并行发起多条路径，谁先有结论就用谁（典型：主链路 + 备用链路取先到者）。
/// 与 `WhenAny` 的差别：race 里「先失败」也算结论，any 只认「先兑现」。
///
/// @warning 「先到」取决于各子 promise 实际落定的时刻与送达顺序（跨执行器时不保证与参数顺序一致）。
///          框架不取消落败的分支，它们会继续跑完（结果被忽略）。
///
/// @note 其余语义同 `WhenAll`；空集合 → 立即以框架侧拒绝「未指定原因」收口（race 无结果可用）。
///
/// @tparam TContext 聚合 promise 的上下文类型（由 spContext 推导）。
/// @tparam TChild 子 promise 类型 / 子 promise 列表类型。
/// @param spContext 聚合 promise 的共享上下文（「必传」）。
/// @param child 子 promise，或 `std::vector<CPromise<同上下文>>`（数量运行时确定）；两者可混用。
/// @return 聚合 promise 句柄（由首个落定的子 promise 驱动）。
template <typename TContext, typename... TChild>
CPromise<TContext> CAsyncExecutor::WhenRace(const std::shared_ptr<TContext>& spContext, const TChild&... child)
{
    return detail::Gather(*this, spContext, detail::kGatherRace, child...);
}

/// @brief 组合器（对齐 JS `Promise.any`）：「首个兑现」的子 promise 定结果；全部拒绝才失败。
///
/// 用法：多条等价路径取「第一个成功的」（典型：多副本 / 多后端取先返回成功者）；
/// 全部失败时以「首个拒绝码」收口（JS 是 AggregateError，本框架用码表达）。
///
/// @note 其余语义同 `WhenAll`；空集合 → 立即以框架侧拒绝「未指定原因」收口（不可能有兑现者）。
///
/// @tparam TContext 聚合 promise 的上下文类型（由 spContext 推导）。
/// @tparam TChild 子 promise 类型 / 子 promise 列表类型。
/// @param spContext 聚合 promise 的共享上下文（「必传」）。
/// @param child 子 promise，或 `std::vector<CPromise<同上下文>>`（数量运行时确定）；两者可混用。
/// @return 聚合 promise 句柄（由首个兑现的子 promise 驱动）。
template <typename TContext, typename... TChild>
CPromise<TContext> CAsyncExecutor::WhenAny(const std::shared_ptr<TContext>& spContext, const TChild&... child)
{
    return detail::Gather(*this, spContext, detail::kGatherAny, child...);
}

}  // namespace async
}  // namespace common
