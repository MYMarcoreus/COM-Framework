#pragma once

#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <utility>
#include <vector>

#include "Thread/ThreadPool.h"

// ====================================================================
// 无异常版异步框架（Option 风格：有值 / 无值）
// 风格类似 Rust Option / C++ std::optional
//
// 核心语义：
//  - 任务链用「有值 / 无值」表达：
//      有值 → 传播（继续链）
//      无值 → 终止（正常提前结束，不是错误）
//  - 错误不再是框架概念：错误码 / 错误信息就是普通值，由调用方解释；
//    想「出错终止」就返回 no::None，想「错误继续」就返回值
//  - 无异常契约：Submit/Then/Get 不向调用方抛异常；
//    任务内部异常被捕获并转为「无值终止」（内部调试原因 kException）
//  - 扁平化：变换函数可返回 CTask<U>（异步继续），自动平铺
//  - void 任务支持 Then：无参数往下传，可继续链
//
// 用法示例：
// @code
//   common::nothrow::CAsyncExecutor exec(2);
//   exec.Start();
//   common::nothrow::CTaskResult<int> r =
//       exec.Submit([]() { return 3; })
//           .Then([](int n) { return n * 2; })
//           .Then([](int n) -> common::nothrow::CTaskResult<int> {
//               if (n < 0) return common::nothrow::None;  // 无值 → 终止
//               return n + 1;
//           })
//           .Get();
//   if (r.HasValue()) { /* 有值 */ }
//   else { /* 终止（Reason() 区分原因）*/ }
// @endcode
//
// 特性：
//  - 链式 Then：变换返回「普通值（传播）/ CTaskResult（有值或无值）/ CTask（flatMap）」
//  - 生命周期加固：任务链通过共享句柄引用线程池，执行器析构后任务仍安全完成
//  - 线程模型：任务与续接在工作线程执行；任务已完成时注册的回调在注册线程同步触发
// ====================================================================

namespace common {
namespace nothrow {

// 模板前向声明（detail 里的 TransformKind / UnwrapTask / FlatMapForward 需要）。
template <typename TValue> class CTask;

/// @brief 无值哨兵（对标 Rust None / C++ std::nullopt）。
///        变换函数返回它表示「无值 → 终止链」。
struct CNoneTag
{
};

/// 无值标记（返回 no::None 表示终止）。
const CNoneTag None = CNoneTag();

namespace detail {

/// @brief 终止 / 未完成原因（仅用于调试区分，不参与类型系统）。
enum CTaskEndReason
{
    kEndCompleted = 0, // 正常完成（有值传播 / void 完成）。
    kEndNone,          // 业务返回 None 终止。
    kNotStarted,       // 执行器未启动（Submit 时线程池不可用）。
    kStopped,          // 执行器已停止（续接投递被拒）。
    kException         // 任务/变换抛出异常（已被框架捕获转为无值终止）。
};

} // namespace detail

/// @brief 任务结果（Option 风格：Some(value) | None）。
///
/// 一个结果要么「有值」要么「无值」：
///  - HasValue() 为 true：可经 Value() 取有值；
///  - HasValue() 为 false：链终止（Reason() 区分终止原因，调试用）。
/// 有值通过「从值隐式构造 / CTaskResult(value)」表达；
/// 无值通过「默认构造 / CTaskResult(no::None) / 返回 no::None」表达。
///
/// @tparam TValue 有值时携带的值类型。
template <typename TValue>
class CTaskResult
{
public:
    /// 默认构造：无值（None，业务终止）。
    CTaskResult() : m_pValue(), m_reason(detail::kEndNone) {}

    /// 显式无值（return no::None;）。
    CTaskResult(CNoneTag) : m_pValue(), m_reason(detail::kEndNone) {}

    /// 从值隐式构造有值（Some）。
    CTaskResult(const TValue& value)
        : m_pValue(new TValue(value)), m_reason(detail::kEndCompleted) {}

    /// 从值移动构造有值（Some，支持 move-only / 减少拷贝）。
    CTaskResult(TValue&& value)
        : m_pValue(new TValue(std::move(value))), m_reason(detail::kEndCompleted) {}

    /// 是否有值（Some）。
    bool HasValue() const { return m_pValue != nullptr; }

    /// 有值时的值（仅在 HasValue() 为 true 时调用）。
    const TValue& Value() const { return *m_pValue; }

    /// 终止原因（调试用；HasValue() 为 false 时区分原因）。
    detail::CTaskEndReason Reason() const { return m_reason; }

    /// 便捷写法：if (result)。
    explicit operator bool() const { return HasValue(); }

    /// 有值返回值，无值返回 defValue。
    ///
    /// @param defValue 无值时的默认值。
    TValue ValueOr(const TValue& defValue) const
    {
        return HasValue() ? *m_pValue : defValue;
    }

    /// 内部：指定原因的无值结果（框架内部错误 / 终止原因用）。
    static CTaskResult MakeNone(detail::CTaskEndReason reason)
    {
        CTaskResult r;
        r.m_reason = reason;
        return r;
    }

private:
    std::shared_ptr<TValue> m_pValue; // 有值（非空 ⇔ Some）。
    detail::CTaskEndReason m_reason;  // 终止原因（调试）。
};

/// @brief CTaskResult&lt;void&gt; 特化：无值即完成。
///
/// void 任务没有「有值/无值」之分，只有「完成 / 终止」：
///  - HasValue() 为 true = 完成（正常结束）；
///  - HasValue() 为 false = 被上游终止（Reason() 区分原因）。
template <>
class CTaskResult<void>
{
public:
    /// 默认构造：完成。
    CTaskResult() : m_reason(detail::kEndCompleted) {}

    /// 显式终止（return no::None;）。
    CTaskResult(CNoneTag) : m_reason(detail::kEndNone) {}

    /// 是否完成（true = 正常结束）。
    bool HasValue() const { return m_reason == detail::kEndCompleted; }

    /// 便捷写法：if (result)。
    explicit operator bool() const { return HasValue(); }

    /// 终止原因（调试用）。
    detail::CTaskEndReason Reason() const { return m_reason; }

    /// 内部：指定原因的无值结果。
    static CTaskResult MakeNone(detail::CTaskEndReason reason)
    {
        CTaskResult r;
        r.m_reason = reason;
        return r;
    }

private:
    detail::CTaskEndReason m_reason; // 完成/终止原因（调试）。
};

namespace detail {

/// @brief 任务共享状态（Option 版）：结果 + 续接列表 + 同步等待。
///
/// @tparam TValue 任务结果的值类型。
template <typename TValue>
class CTaskState
{
public:
    /// 续接回调：接收最终结果。
    using Continuation = std::function<void(const CTaskResult<TValue>&)>;

    /// 创建状态（初始未就绪）。
    CTaskState() : m_bReady(false) {}

    /// 完成并触发续接（锁外调用续接，防重入死锁）。
    ///
    /// 仅首次生效；先唤醒 Wait，再按注册顺序在锁外调用所有续接。
    void Complete(const CTaskResult<TValue>& result)
    {
        std::vector<Continuation> vecCbs;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_bReady)
            {
                return;
            }
            m_bReady = true;
            m_result = result;
            vecCbs.swap(m_vecContinuations);
        }
        m_cv.notify_all();
        for (size_t i = 0; i < vecCbs.size(); ++i)
        {
            if (vecCbs[i])
            {
                vecCbs[i](result);
            }
        }
    }

    /// 注册续接；已就绪则立即触发。
    void AddContinuation(const Continuation& fnCallback)
    {
        bool bFireNow = false;
        CTaskResult<TValue> result;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_bReady)
            {
                bFireNow = true;
                result = m_result;
            }
            else
            {
                m_vecContinuations.push_back(fnCallback);
            }
        }
        if (bFireNow && fnCallback)
        {
            fnCallback(result);
        }
    }

    /// 阻塞等待结果。
    auto Wait() -> CTaskResult<TValue>
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]() { return m_bReady; });
        return m_result;
    }

private:
    std::mutex m_mutex;                  // 保护状态与续接列表。
    std::condition_variable m_cv;        // 通知 Wait 等待者。
    std::vector<Continuation> m_vecContinuations; // 续接列表（未完成时）。
    bool m_bReady;                       // 是否已完成。
    CTaskResult<TValue> m_result;        // 最终结果（完成后有效）。
};

/// @brief CTaskState&lt;void&gt; 特化。
template <>
class CTaskState<void>
{
public:
    using Continuation = std::function<void(const CTaskResult<void>&)>;

    CTaskState() : m_bReady(false) {}

    void Complete(const CTaskResult<void>& result)
    {
        std::vector<Continuation> vecCbs;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_bReady)
            {
                return;
            }
            m_bReady = true;
            m_result = result;
            vecCbs.swap(m_vecContinuations);
        }
        m_cv.notify_all();
        for (size_t i = 0; i < vecCbs.size(); ++i)
        {
            if (vecCbs[i])
            {
                vecCbs[i](result);
            }
        }
    }

    void AddContinuation(const Continuation& fnCallback)
    {
        bool bFireNow = false;
        CTaskResult<void> result;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_bReady)
            {
                bFireNow = true;
                result = m_result;
            }
            else
            {
                m_vecContinuations.push_back(fnCallback);
            }
        }
        if (bFireNow && fnCallback)
        {
            fnCallback(result);
        }
    }

    auto Wait() -> CTaskResult<void>
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]() { return m_bReady; });
        return m_result;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<Continuation> m_vecContinuations;
    bool m_bReady;
    CTaskResult<void> m_result;
};

/// @brief 以「有值」完成状态（TValue 非 void，携带 f() 的返回值）。
template <typename TResult, typename TFn>
void CompleteSuccess(const std::shared_ptr<CTaskState<TResult> >& pState, TFn f)
{
    pState->Complete(CTaskResult<TResult>(f())); // f() 结果 → Some（隐式）。
}

/// @brief 以「完成」结束状态（TValue 为 void，执行 f 但不携带值）。
template <typename TFn>
void CompleteSuccess(const std::shared_ptr<CTaskState<void> >& pState, TFn f)
{
    f();
    pState->Complete(CTaskResult<void>()); // 完成。
}

/// @brief 调用结果类型（C++11 兼容，替代已弃用的 std::result_of）。
template <typename TFn, typename... TArgs>
struct TInvokeResult
{
    using type = decltype(std::declval<TFn>()(std::declval<TArgs>()...));
};

/// @brief 变换函数返回类型的分类（分派 RunTransform 用）：
///        0 = 普通值；1 = CTask（flatMap）；2 = CTaskResult（结果原样转发）。
template <typename T>
struct TransformKind
{
    using type = std::integral_constant<int, 0>;
};

template <typename U>
struct TransformKind<CTask<U> >
{
    using type = std::integral_constant<int, 1>;
};

template <typename U>
struct TransformKind<CTaskResult<U> >
{
    using type = std::integral_constant<int, 2>;
};

/// @brief 解包任务类型：CTask&lt;U&gt; / CTaskResult&lt;U&gt; -&gt; U；其它 T -&gt; T。
template <typename T>
struct UnwrapTask
{
    using type = T;
};

template <typename U>
struct UnwrapTask<CTask<U> >
{
    using type = U;
};

template <typename U>
struct UnwrapTask<CTaskResult<U> >
{
    using type = U;
};

/// @brief 执行器句柄（生命周期加固核心）。
struct CExecutorHandle
{
    std::shared_ptr<common::CThreadPool> m_pPool; // 工作线程池（任务链持有时不释放）。
    std::atomic<bool> m_bStopped;                 // 是否已停止（停止后拒绝新投递）。

    CExecutorHandle() : m_bStopped(false) {}
};

/// @brief 转发内部任务结果（TNew 非 void）。
template <typename TNew>
void FlatMapForward(const std::shared_ptr<CTaskState<TNew> >& pNextState,
                    CTask<TNew>& inner, std::false_type)
{
    inner.OnSuccess([pNextState](const TNew& value)
    {
        pNextState->Complete(CTaskResult<TNew>(value));
    });
    inner.OnNone([pNextState](CTaskEndReason reason)
    {
        pNextState->Complete(CTaskResult<TNew>::MakeNone(reason));
    });
}

/// @brief 转发内部任务结果（TNew 为 void）。
template <typename TNew>
void FlatMapForward(const std::shared_ptr<CTaskState<TNew> >& pNextState,
                    CTask<TNew>& inner, std::true_type)
{
    inner.OnSuccess([pNextState]()
    {
        pNextState->Complete(CTaskResult<TNew>());
    });
    inner.OnNone([pNextState](CTaskEndReason reason)
    {
        pNextState->Complete(CTaskResult<TNew>::MakeNone(reason));
    });
}

/// @brief 执行变换（有参，变换返回普通值）：成功则完成下游。
template <typename TOut, typename TFn, typename TValue>
void RunTransform(const std::shared_ptr<CTaskState<TOut> >& pNextState,
                  TFn f, TValue valueCopied, std::integral_constant<int, 0>)
{
    try
    {
        pNextState->Complete(CTaskResult<TOut>(f(valueCopied)));
    }
    catch (const std::exception&)
    {
        pNextState->Complete(CTaskResult<TOut>::MakeNone(kException));
    }
    catch (...)
    {
        pNextState->Complete(CTaskResult<TOut>::MakeNone(kException));
    }
}

/// @brief 执行变换（有参，变换返回 CTask）：扁平化 flatMap。
template <typename TNew, typename TFn, typename TValue>
void RunTransform(const std::shared_ptr<CTaskState<TNew> >& pNextState,
                  TFn f, TValue valueCopied, std::integral_constant<int, 1>)
{
    CTask<TNew> inner;
    try
    {
        inner = f(valueCopied);
    }
    catch (const std::exception&)
    {
        pNextState->Complete(CTaskResult<TNew>::MakeNone(kException));
        return;
    }
    catch (...)
    {
        pNextState->Complete(CTaskResult<TNew>::MakeNone(kException));
        return;
    }
    FlatMapForward(pNextState, inner, typename std::is_same<TNew, void>::type());
}

/// @brief 执行变换（有参，变换返回 CTaskResult）：结果原样转发（Some/None）。
template <typename TOut, typename TFn, typename TValue>
void RunTransform(const std::shared_ptr<CTaskState<TOut> >& pNextState,
                  TFn f, TValue valueCopied, std::integral_constant<int, 2>)
{
    try
    {
        pNextState->Complete(f(valueCopied)); // 原样转发（有值/无值）。
    }
    catch (const std::exception&)
    {
        pNextState->Complete(CTaskResult<TOut>::MakeNone(kException));
    }
    catch (...)
    {
        pNextState->Complete(CTaskResult<TOut>::MakeNone(kException));
    }
}

/// @brief 执行变换（无参，void 上游的 Then 用）：变换返回普通值。
template <typename TOut, typename TFn>
void RunTransformVoid(const std::shared_ptr<CTaskState<TOut> >& pNextState,
                      TFn f, std::integral_constant<int, 0>)
{
    try
    {
        pNextState->Complete(CTaskResult<TOut>(f()));
    }
    catch (const std::exception&)
    {
        pNextState->Complete(CTaskResult<TOut>::MakeNone(kException));
    }
    catch (...)
    {
        pNextState->Complete(CTaskResult<TOut>::MakeNone(kException));
    }
}

/// @brief 执行变换（无参，void 上游的 Then 用）：变换返回 CTask（flatMap）。
template <typename TNew, typename TFn>
void RunTransformVoid(const std::shared_ptr<CTaskState<TNew> >& pNextState,
                      TFn f, std::integral_constant<int, 1>)
{
    CTask<TNew> inner;
    try
    {
        inner = f();
    }
    catch (const std::exception&)
    {
        pNextState->Complete(CTaskResult<TNew>::MakeNone(kException));
        return;
    }
    catch (...)
    {
        pNextState->Complete(CTaskResult<TNew>::MakeNone(kException));
        return;
    }
    FlatMapForward(pNextState, inner, typename std::is_same<TNew, void>::type());
}

/// @brief 执行变换（无参，void 上游的 Then 用）：变换返回 CTaskResult。
template <typename TOut, typename TFn>
void RunTransformVoid(const std::shared_ptr<CTaskState<TOut> >& pNextState,
                      TFn f, std::integral_constant<int, 2>)
{
    try
    {
        pNextState->Complete(f());
    }
    catch (const std::exception&)
    {
        pNextState->Complete(CTaskResult<TOut>::MakeNone(kException));
    }
    catch (...)
    {
        pNextState->Complete(CTaskResult<TOut>::MakeNone(kException));
    }
}

} // namespace detail

/// @brief 异步任务（Option 风格），支持链式调用（Then）。
///
/// 由 CAsyncExecutor::Submit / CTask::FromResult 创建；
/// 通过 Then 串联后续步骤，Get 阻塞获取最终结果（CTaskResult，不抛异常）。
/// 上游无值（终止）时 Then 不再执行，终止原因传播给下游。
///
/// @tparam TValue 任务携带的值类型。
template <typename TValue>
class CTask
{
public:
    /// 创建空任务（未完成）。
    CTask()
        : m_pExecutor(),
          m_pState(std::make_shared<detail::CTaskState<TValue> >())
    {
    }

    /// 从已就绪结果创建任务（可启动链式调用）。
    static auto FromResult(const CTaskResult<TValue>& result) -> CTask<TValue>
    {
        CTask<TValue> task;
        task.m_pState->Complete(result);
        return task;
    }

    /// 链式续接：上游有值时执行 fnTransform(value)，上游无值则终止传播。
    ///
    /// 变换函数返回三种：
    ///  - 普通值 TNew：有值传播（Then → Then → Get）；
    ///  - CTaskResult&lt;TNew&gt;：有值传播 / 无值（None）终止；
    ///  - CTask&lt;TNew&gt;：扁平化（flatMap），内部任务完成后转发其结果。
    template <typename TFn>
    auto Then(TFn fnTransform)
        -> CTask<typename detail::UnwrapTask<
            typename detail::TInvokeResult<TFn, TValue>::type>::type>;

    /// 阻塞获取最终结果（不抛异常）。
    auto Get() const -> CTaskResult<TValue> { return m_pState->Wait(); }

    /// 注册成功回调（有值时触发）。
    void OnSuccess(const std::function<void(const TValue&)>& fnCallback);

    /// 注册无值回调（链终止时触发；参数为终止原因，调试用）。
    void OnNone(const std::function<void(detail::CTaskEndReason)>& fnCallback);

private:
    template <typename U> friend class CTask;
    friend class CAsyncExecutor;

    std::shared_ptr<detail::CExecutorHandle> m_pExecutor; // 执行器句柄（续接投递用，可空）。
    std::shared_ptr<detail::CTaskState<TValue> > m_pState; // 任务共享状态。
};

/// @brief 异步任务（Option 风格，TValue 为 void 的特化）。
///
/// 用于无返回值任务：也支持 Then（变换函数无参数，可继续链）。
/// OnSuccess 回调无参数。
template <>
class CTask<void>
{
public:
    /// 创建空任务（未完成）。
    CTask()
        : m_pExecutor(),
          m_pState(std::make_shared<detail::CTaskState<void> >())
    {
    }

    /// 从已就绪结果创建任务。
    static auto FromResult(const CTaskResult<void>& result) -> CTask<void>
    {
        CTask<void> task;
        task.m_pState->Complete(result);
        return task;
    }

    /// 链式续接：上游完成时执行 fnTransform()（无参数），上游终止则传播。
    template <typename TFn>
    auto Then(TFn fnTransform)
        -> CTask<typename detail::UnwrapTask<
            typename detail::TInvokeResult<TFn>::type>::type>;

    /// 阻塞获取最终结果（不抛异常）。
    auto Get() const -> CTaskResult<void> { return m_pState->Wait(); }

/// @brief 注册完成回调（无参数）。
    void OnSuccess(const std::function<void()>& fnCallback);

    /// 注册无值回调（链终止时触发；参数为终止原因，调试用）。
    void OnNone(const std::function<void(detail::CTaskEndReason)>& fnCallback);

private:
    template <typename U> friend class CTask;
    friend class CAsyncExecutor;

    std::shared_ptr<detail::CExecutorHandle> m_pExecutor; // 执行器句柄（续接投递用，可空）。
    std::shared_ptr<detail::CTaskState<void> > m_pState;  // 任务共享状态。
};
///
/// 基于线程池执行任务，支持链式调用（Submit → Then → Get）。
/// 无模板参数（错误不再是框架概念）。
///
/// @note 生命周期：任务链通过共享句柄引用执行器线程池；执行器析构后，
///       已投递/已链式任务仍安全完成，新投递以无值（kStopped）完成。
class CAsyncExecutor
{
public:
    /// 创建执行器（指定工作线程数，默认 1）。
    explicit CAsyncExecutor(size_t nThreadCount = 1);

    /// 销毁执行器（停止并等待任务完成）。
    ~CAsyncExecutor();

    /// 启动工作线程。
    ///
    /// @return true 启动成功；false 已启动或线程数为 0。
    bool Start();

    /// 提交任务并返回 CTask（任务内部异常自动转为无值终止）。
    ///
    /// @tparam TFn 任务函数类型（返回值作为任务结果）。
    /// @param f 任务函数（在工作线程上执行）。
    /// @return 关联本执行器的任务；执行器未启动时任务立即以无值（kNotStarted）完成。
    template <typename TFn>
    auto Submit(TFn f) -> CTask<typename detail::TInvokeResult<TFn>::type>;

    /// 提交无返回值任务（fire-and-forget）。
    ///
    /// @return true 提交成功；false 执行器未启动。
    bool Post(const std::function<void()>& fnTask);

    /// 停止并等待任务完成（优雅关闭）。
    void Stop();

    /// 是否正在运行。
    bool IsRunning() const;

private:
    std::shared_ptr<detail::CExecutorHandle> m_pHandle; // 执行器句柄（任务链共享）。
    size_t m_nThreadCount;                              // 工作线程数。
};

// ================= 模板实现 ================

/// @brief Then 实现（非 void 上游）：注册上游续接，有值则投递变换，无值则传播终止。
template <typename TValue>
template <typename TFn>
auto CTask<TValue>::Then(TFn f)
    -> CTask<typename detail::UnwrapTask<
        typename detail::TInvokeResult<TFn, TValue>::type>::type>
{
    using TResult = typename detail::TInvokeResult<TFn, TValue>::type; // 变换原始返回类型。
    using TOut = typename detail::UnwrapTask<TResult>::type;           // 解包后下游结果类型。
    CTask<TOut> taskNext;
    taskNext.m_pExecutor = m_pExecutor; // 沿用上游执行器句柄（续接投递用）。
    auto pNextState = taskNext.m_pState;
    auto pExecutor = m_pExecutor;

    m_pState->AddContinuation(
        [pExecutor, pNextState, f](const CTaskResult<TValue>& upResult)
        {
            // ① 上游无值：终止传播（原因透传）。
            if (!upResult.HasValue())
            {
                pNextState->Complete(CTaskResult<TOut>::MakeNone(upResult.Reason()));
                return;
            }
            // ② 拷贝值，供异步续接安全使用（不引用上游共享状态）。
            TValue valueCopied = upResult.Value();
            std::function<void()> fnRun = [pNextState, f, valueCopied]()
            {
                // ③ 执行变换：普通值 → 传播；CTask → flatMap；CTaskResult → 原样转发。
                detail::RunTransform(pNextState, f, valueCopied,
                    typename detail::TransformKind<TResult>::type());
            };
            // ④ 在执行器上执行；无执行器时内联执行。
            if (pExecutor != nullptr && pExecutor->m_pPool != nullptr)
            {
                if (pExecutor->m_bStopped || !pExecutor->m_pPool->Submit(fnRun))
                {
                    pNextState->Complete(CTaskResult<TOut>::MakeNone(detail::kStopped));
                }
            }
            else
            {
                fnRun();
            }
        });
    return taskNext;
}

/// @brief Then 实现（void 上游）：上游完成则执行 fn()（无参），终止则传播。
template <typename TFn>
auto CTask<void>::Then(TFn f)
    -> CTask<typename detail::UnwrapTask<
        typename detail::TInvokeResult<TFn>::type>::type>
{
    using TResult = typename detail::TInvokeResult<TFn>::type;
    using TOut = typename detail::UnwrapTask<TResult>::type;
    CTask<TOut> taskNext;
    taskNext.m_pExecutor = m_pExecutor;
    auto pNextState = taskNext.m_pState;
    auto pExecutor = m_pExecutor;

    m_pState->AddContinuation(
        [pExecutor, pNextState, f](const CTaskResult<void>& upResult)
        {
            // ① 上游终止：传播。
            if (!upResult.HasValue())
            {
                pNextState->Complete(CTaskResult<TOut>::MakeNone(upResult.Reason()));
                return;
            }
            // ② 执行变换（fn 无参数）。
            std::function<void()> fnRun = [pNextState, f]()
            {
                detail::RunTransformVoid(pNextState, f,
                    typename detail::TransformKind<TResult>::type());
            };
            if (pExecutor != nullptr && pExecutor->m_pPool != nullptr)
            {
                if (pExecutor->m_bStopped || !pExecutor->m_pPool->Submit(fnRun))
                {
                    pNextState->Complete(CTaskResult<TOut>::MakeNone(detail::kStopped));
                }
            }
            else
            {
                fnRun();
            }
        });
    return taskNext;
}

/// @brief OnSuccess 实现（非 void）。
template <typename TValue>
void CTask<TValue>::OnSuccess(const std::function<void(const TValue&)>& fnCallback)
{
    m_pState->AddContinuation(
        [fnCallback](const CTaskResult<TValue>& result)
        {
            if (result.HasValue() && fnCallback)
            {
                fnCallback(result.Value());
            }
        });
}

/// @brief OnNone 实现（非 void）。
template <typename TValue>
void CTask<TValue>::OnNone(const std::function<void(detail::CTaskEndReason)>& fnCallback)
{
    m_pState->AddContinuation(
        [fnCallback](const CTaskResult<TValue>& result)
        {
            if (!result.HasValue() && fnCallback)
            {
                fnCallback(result.Reason());
            }
        });
}

/// @brief Submit 实现：把任务函数投递到线程池执行。
template <typename TFn>
auto CAsyncExecutor::Submit(TFn f)
    -> CTask<typename detail::TInvokeResult<TFn>::type>
{
    using TResult = typename detail::TInvokeResult<TFn>::type; // 任务结果类型。
    CTask<TResult> task;
    task.m_pExecutor = m_pHandle; // 共享执行器句柄（任务链持有时线程池不释放）。
    auto pState = task.m_pState;
    auto pHandle = m_pHandle;
    std::function<void()> fnRun = [pState, f]()
    {
        try
        {
            // 执行任务；成功完成（有值或 void 完成）。f 可能抛异常：捕获并转为无值终止。
            detail::CompleteSuccess(pState, f);
        }
        catch (const std::exception&)
        {
            pState->Complete(CTaskResult<TResult>::MakeNone(detail::kException));
        }
        catch (...)
        {
            pState->Complete(CTaskResult<TResult>::MakeNone(detail::kException));
        }
    };
    if (!pHandle || pHandle->m_bStopped || !pHandle->m_pPool || !pHandle->m_pPool->Submit(fnRun))
    {
        // 执行器不可用：任务立即以无值（未启动）完成。
        pState->Complete(CTaskResult<TResult>::MakeNone(detail::kNotStarted));
    }
    return task;
}

} // namespace nothrow
} // namespace common
