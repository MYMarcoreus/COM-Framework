#pragma once

#include <functional>
#include <memory>

#include "Assert.h"
#include "Async/AsyncExecutor.h"
#include "Async/PromiseLayer.h"
#include "Async/PromiseResult.h"
#include "Async/PromiseState.h"
#include "Async/PromiseTypes.h"

// ====================================================================
// promise 共享核心（`detail::CPromiseCore`）
//
// 从 Promise.h 拆出（2026-09-26）：一条链的所有层共用同一个核心（同一上下文 + 同一执行器），
// 句柄持有者彼此保活（执行器析构后链仍安全跑完）。它只做两件事：
//   - 造任务体：`MakeThenRunner` / `MakeResultRunner`（层语义在 PromiseLayer.h）；
//   - 派发：就地内联 / 按本层类别过门投递（策略**唯一**收敛在 `detail::DispatchInlineOrPost`），
//     派发失败（执行器已停）→ 本层以框架侧拒绝收口，绝不让它永远 pending。
//
// 注意它**不保存类别**：类别只存在层上（`CPromiseState::Kind()`），同一条链的层可以读 / 写 / 直投
// 混排 —— 「链的类别」并不存在；起链时给首层的那个类别是**参数**，用完即弃。
// ====================================================================

namespace common {
namespace async {

namespace detail {

/// @brief promise 共享核心：共享上下文 + 执行器句柄（**不保存类别**）。
///
/// 一条链的所有层共用同一个核心（同一上下文 + 同一执行器），句柄持有者彼此
/// 保活（执行器析构后链仍安全跑完）。
///
/// 「类别」只存在层上（`CPromiseState::Kind()`）：一层一个，同一条链的层可以读 / 写 / 直投混排 ——
/// 所以「链的类别」并不存在，核心也就不存它：起链时给首层的那个类别是**参数**，用完即弃。
///
/// 「本层怎么跑」的「调度策略」（就地内联 / 投递、内联深度限额、读写门准入）归属执行器侧
/// （`detail::ShouldInline` / `detail::DispatchInlineOrPost`，在 AsyncExecutor.h）；
/// 这里只做两件事：「造任务体」（`MakeThenRunner` / `MakeResultRunner`，层语义）
/// 与「失败收口」（框架侧拒绝「执行器已停」）。
///
/// 注：首层不走这里 —— 「起链即强制投递」是 `CPromise::StartChain` 的一条直路
/// （没有调度选择，也就没有分派器）。
template <typename TContext>
class CPromiseCore
{
public:
    /// @brief 创建核心。
    ///
    /// @param pHandle 执行器句柄（可为空：协程构造时尚未绑定执行器，`Start` 时注入）。
    /// @param spContext 共享上下文（「必传」；调用方负责在建链前备好数据，框架不管它的生命周期）。
    CPromiseCore(const std::shared_ptr<CExecutorHandle>& pHandle, const std::shared_ptr<TContext>& spContext)
        : m_pHandle(pHandle), m_spContext(spContext)
    {
        // 上下文强制传入：没有它就无从「共享」——断言把这一契约钉在唯一入口上。
        ASSERT_MSG(spContext != nullptr, "共享上下文必须由调用方传入（框架不做懒创建）");
    }

    /// @brief 共享上下文（恒非空、构造后只读）。
    ///
    /// 上下文是「强制传入」的（没有懒创建 —— 懒创建要让上下文可默认构造、要给一个 "可能还没准备好" 的
    /// 时间窗加锁，而收益只是省掉调用方一行 `make_shared`）。因此热路径（每层都会取一次）
    /// 可以直接取用，既不加锁、也不拷贝 `shared_ptr`。
    ///
    /// @return 共享上下文（引用在核心存活期内有效；要留到别的线程请自行拷贝一份）。
    const std::shared_ptr<TContext>& Context() const
    {
        return m_spContext;
    }

    /// @brief 执行器句柄（投递用）。
    const std::shared_ptr<CExecutorHandle>& Handle() const
    {
        return m_pHandle;
    }

    /// @brief 绑定执行器句柄（协程 Start 时注入）。
    ///
    /// @param pHandle 执行器句柄。
    void SetHandle(const std::shared_ptr<CExecutorHandle>& pHandle)
    {
        m_pHandle = pHandle;
    }

    /// @brief 级联执行下一层（then 语义：处理器看不到上游结果）。
    ///
    /// 「只有当前线程已经是本链执行器的线程」时才就地内联（省一次入队 + 保序）；
    /// 否则一律投递回本链执行器（典型场景：被调模块 settle 本链的层，本层就回到本模块线程执行）。
    /// 内联深度也只在同一执行器线程内累加，跨模块不会涨栈。
    ///
    /// @param pState 本层状态。
    /// @param fnHandler 处理器（then 签名）。
    void RunThenHandler(const std::shared_ptr<CPromiseState>& pState, const ThenHandler<TContext>& fnHandler) const
    {
        // 两步：① 造本层任务体（then 语义：处理器只接上下文）；② 交给派发器（就地 / 投递）。
        Dispatch(pState, MakeThenRunner(m_pHandle, Context(), pState, fnHandler));
    }

    /// @brief 级联执行下一层（catch / finally 语义：处理器拿到上游结果）。
    ///
    /// @param pState 本层状态。
    /// @param fnHandler 处理器（catch / finally 签名）。
    /// @param upResult 上一层结果。
    /// @param eMode 处理器模式（catch / finally）。
    void RunResultHandler(const std::shared_ptr<CPromiseState>& pState, const ResultHandler<TContext>& fnHandler,
        const CPromiseResult& upResult, HandlerMode eMode) const
    {
        // 两步：① 造本层任务体（catch / finally 语义：要传上游结果与模式）；② 交给派发器。
        Dispatch(pState, MakeResultRunner(m_pHandle, Context(), pState, fnHandler, upResult, eMode));
    }

    /// @brief 派发「本层的动作体」——「等子链」那一层（`ThenPromise` / `ThenBridge`）用。
    ///
    /// 那一层没有普通 handler，它的动作是「起子链 + 登记回调」（`Adopt`）。它与层体一样会跑
    /// **用户代码**（子链工厂 `fnCreate`），所以必须走同一套调度（就地 / 按本层类别过门投递）：
    /// 上游若由**外部线程** settle（starter 链 / 桥接子链 / 定时器回调），登记路径直接调工厂就会在
    /// 那个线程上、**无槽位**地跑（声明的类别形同虚设）；过门后「本层以什么身份进模块」恒成立，
    /// 工厂也永远跑在本链执行器线程上（与 `ThenBridge` 的文档一致）。
    ///
    /// @param pState 本层状态（类别取自它）。
    /// @param fnRun 本层动作体。
    void DispatchAction(const std::shared_ptr<CPromiseState>& pState, std::function<void()> fnRun) const
    {
        Dispatch(pState, std::move(fnRun));
    }

private:
    /// @brief 派发已造好的任务体：就地内联 / 投递回本链执行器；执行器不可用 → 本层以框架侧失败收口。
    ///
    /// @param pState 本层状态。
    /// @param fnRun 任务体。
    void Dispatch(const std::shared_ptr<CPromiseState>& pState, std::function<void()> fnRun) const
    {
        ASSERT(pState != nullptr);  // 内部调用：本层状态恒存在。

        // ① 派发：策略（已在本链执行器线程 + 持本门同类槽位 + 无人在排队 → 就地；否则按类别过门投递；
        //    超过内联深度也改投递）由执行器侧决定；类别取**本层**的（同一条链的层可以不同）。
        const bool bDispatched = DispatchInlineOrPost(Handle(), pState->Kind(), std::move(fnRun));

        // ② 派发失败（执行器已停 / 拒绝投递）→ 本层以框架侧失败收口，绝不让它永远 pending。
        if (!bDispatched)
        {
            pState->Settle(CPromiseResult::Reject(std::runtime_error("执行器已停")));
        }
    }

private:
    std::shared_ptr<CExecutorHandle> m_pHandle;  ///< 执行器句柄。
    std::shared_ptr<TContext> m_spContext;       ///< 共享上下文（构造时传入，之后只读）。
};

}  // namespace detail
}  // namespace async
}  // namespace common
