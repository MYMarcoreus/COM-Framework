#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <utility>
#include <vector>

#include "Thread/ThreadPool.h"

// ====================================================================
// 回调调试追踪（仅调试构建有效）
//
// 仅在非优化调试构建（make debug / -O0，即未定义 __OPTIMIZE__）下启用：
// 任务完成触发续接、回调投递时打印当前线程 ID 与调用栈（backtrace），
// 便于定位异步回调的触发链路（谁在哪个线程触发了这个回调）。
// 发布构建（-O2，定义 __OPTIMIZE__）自动编译为空实现，零运行时开销。
// ====================================================================
#if defined(__linux__) && !defined(__OPTIMIZE__)
    #define NOTHROW_DEBUG_TRACE 1
    #include <execinfo.h>
    #include <cstdio>
    #include <cstdlib>
    #include <thread>
#endif

// ====================================================================
// 任务调试标签宏（NOTHROW_TAG）
//
// 用于给任务打调试标签：调用 Submit/Then 时作为第二个参数传入。
// 调试构建（make debug / -O0）下，回调栈追踪（TraceInvoke）会输出
// task=<标签>，便于定位「当前任务/回调是哪个函数注册的」。
//
// 用法：
//   exec.Submit(fn, NOTHROW_TAG)   // 标签 = 当前函数名（__PRETTY_FUNCTION__）
//   exec.Submit(fn, "fetch-user") // 自定义标签（同一函数内区分多个任务）
//   task.Then(fn, NOTHROW_TAG)     // Then 也可打标签（标记下游任务）
//
// 说明：
//   - NOTHROW_TAG 展开为 __PRETTY_FUNCTION__（注册点函数名，含签名）。
//     它是编译期静态字符串，以 const char* 存储：零分配、程序生命周期安全。
//   - 同一函数内多个调用点的 __PRETTY_FUNCTION__ 相同，如需逐任务区分
//     请用自定义字符串标签（如 "fetch-user"、"write-log"）。
//   - 发布构建（-O2）下 NOTHROW_TAG 退化为 NULL；此时标签不存储、
//     TraceInvoke 为空实现，零运行时开销。
// ====================================================================
#if defined(NOTHROW_DEBUG_TRACE)
    #define NOTHROW_TAG __PRETTY_FUNCTION__
#else
    #define NOTHROW_TAG NULL
#endif

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
//
// 特性：
//  - 链式 Then：变换返回「普通值（传播）/ CTaskResult（有值或无值）/ CTask（flatMap）」
//  - 生命周期加固：任务链通过共享句柄引用线程池，执行器析构后任务仍安全完成
//  - 线程模型：任务与续接在工作线程执行；任务已完成时注册的回调投递到执行器异步触发
//    （执行器未启动/已停止时视为调用失败）
// ====================================================================

namespace common {
namespace nothrow {

// 前向声明（detail 里的 TaskTraits / FlatMapForward / CTaskBase 需要）。
template <typename TValue> class CTask;
class CAsyncExecutor; // 前向声明（CTaskBase / CTask 的 friend）。

/// @brief 无值哨兵（对标 Rust None / C++ std::nullopt）。
///        变换函数返回它表示「无值 → 终止链」。
struct CNoneTag
{
};

/// @brief 无值标记（返回 no::None 表示终止）。
const CNoneTag None = CNoneTag{};

namespace detail {

/// @brief 终止 / 未完成原因（仅用于调试区分，不参与类型系统）。
enum CTaskEndReason
{
    kEndCompleted = 0, ///< 正常完成（有值传播 / void 完成）。
    kEndNone,          ///< 业务返回 None 终止。
    kNotStarted,       ///< 执行器未启动（Submit 时线程池不可用）。
    kStopped,          ///< 执行器已停止（续接投递被拒）。
    kException         ///< 任务/变换抛出异常（已被框架捕获转为无值终止）。
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
/// 值内联存储（对标 std::optional / Rust Option）：无堆分配、无引用计数。
/// 代价：结果拷贝为深拷贝（对小对象开销远小于堆分配）；结果类型需
/// 「默认构造 + 可拷贝」（move-only 类型如 std::unique_ptr 不支持，
/// 可改用 std::shared_ptr 包裹）。
///
/// @tparam TValue 有值时携带的值类型。
template <typename TValue>
class CTaskResult
{
public:
    /// @brief 默认构造：无值（None，业务终止）。
    CTaskResult() : m_bHasValue(false), m_reason(detail::kEndNone), m_value() {}

    /// @brief 显式无值（return no::None;）。
    CTaskResult(CNoneTag) : m_bHasValue(false), m_reason(detail::kEndNone), m_value() {}

    /// @brief 从值隐式构造有值（Some）。
    ///
    /// @param value 结果值。
    CTaskResult(const TValue& value)
        : m_bHasValue(true), m_reason(detail::kEndCompleted), m_value(value) {}

    /// @brief 从值移动构造有值（Some，减少拷贝）。
    ///
    /// @param value 结果值（移动语义）。
    CTaskResult(TValue&& value)
        : m_bHasValue(true), m_reason(detail::kEndCompleted), m_value(std::move(value)) {}

    /// @brief 是否有值（Some）。
    bool HasValue() const { return m_bHasValue; }

    /// @brief 有值时的值（仅在 HasValue() 为 true 时调用）。
    const TValue& Value() const { return m_value; }

    /// @brief 有值时的可写值（仅在 HasValue() 为 true 时调用）。
    TValue& Value() { return m_value; }

    /// @brief 终止原因（调试用；HasValue() 为 false 时区分原因）。
    detail::CTaskEndReason Reason() const { return m_reason; }

    /// @brief 便捷写法：if (result)。
    explicit operator bool() const { return HasValue(); }

    /// @brief 有值返回值，无值返回 defValue。
    ///
    /// @param defValue 无值时的默认值。
    TValue ValueOr(const TValue& defValue) const
    {
        return HasValue() ? m_value : defValue;
    }

    /// @brief 内部：指定原因的无值结果（框架内部错误 / 终止原因用）。
    ///
    /// @param reason 终止原因。
    static CTaskResult MakeNone(detail::CTaskEndReason reason)
    {
        CTaskResult r;
        r.m_reason = reason;
        return r;
    }

private:
    bool m_bHasValue;                // 是否有值（Some）。
    detail::CTaskEndReason m_reason; // 终止原因（调试）。
    TValue m_value;                  // 内联存储的值（有值时才有效）。
};

/// @brief `CTaskResult<void>` 特化：无值即完成。
///
/// void 任务没有「有值/无值」之分，只有「完成 / 终止」：
///  - HasValue() 为 true = 完成（正常结束）；
///  - HasValue() 为 false = 被上游终止（Reason() 区分原因）。
template <>
class CTaskResult<void>
{
public:
    /// @brief 默认构造：完成。
    CTaskResult() : m_reason(detail::kEndCompleted) {}

    /// @brief 显式终止（return no::None;）。
    CTaskResult(CNoneTag) : m_reason(detail::kEndNone) {}

    /// @brief 是否完成（true = 正常结束）。
    bool HasValue() const { return m_reason == detail::kEndCompleted; }

    /// @brief 便捷写法：if (result)。
    explicit operator bool() const { return HasValue(); }

    /// @brief 终止原因（调试用）。
    detail::CTaskEndReason Reason() const { return m_reason; }

    /// @brief 内部：指定原因的无值结果。
    ///
    /// @param reason 终止原因。
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

/// @brief 打印回调触发调用栈（仅调试构建；发布构建为空操作，零开销）。
///
/// @param szWhat 触发场景描述（如「Complete 触发续接」）。
/// @param nDepth 打印最近 N 帧（<0 表示全部）。
inline void TraceInvoke(const char* szWhat, const char* szTag = NULL, int nDepth = -1)
{
#if defined(NOTHROW_DEBUG_TRACE)
    static const int kMaxFrames = 32;
    void* apBuffer[kMaxFrames];
    int nFrames = backtrace(apBuffer, kMaxFrames);
    char** ppSymbols = backtrace_symbols(apBuffer, nFrames);
    unsigned long nThreadId = static_cast<unsigned long>(
        std::hash<std::thread::id>()(std::this_thread::get_id()));
    std::fprintf(stderr, "\n[nothrow::trace] %s | task=%s | thread=%lu | frames=%d\n",
                 szWhat, (szTag != NULL && szTag[0] != '\0') ? szTag : "<unnamed>",
                 nThreadId, nFrames);
    int nStart = 0;
    if (nDepth >= 0 && nDepth < nFrames)
    {
        nStart = nFrames - nDepth;
    }
    for (int i = nStart; i < nFrames; ++i)
    {
        std::fprintf(stderr, "  %2d  %s\n", i, ppSymbols[i]);
    }
    std::fprintf(stderr, "\n");
    std::free(ppSymbols);
#else
    (void)szWhat;
    (void)szTag;
    (void)nDepth;
#endif
}

// ===========================================================================
// ================================= 内部类 =================================
// ===========================================================================

/// @brief 执行器句柄（生命周期加固核心）。
struct CExecutorHandle
{
    std::shared_ptr<common::CThreadPool> m_pPool; // 工作线程池（任务链持有时不释放）。
    std::atomic<bool> m_bStopped;                 // 是否已停止（停止后拒绝新投递）。

    CExecutorHandle() : m_bStopped(false) {}
};

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
    CTaskState() : m_bReady(false)
#if defined(NOTHROW_DEBUG_TRACE)
        , m_pszTag("")
#endif
    {}

    /// @brief 完成并触发续接（锁外调用续接，防重入死锁）。
    ///
    /// 仅首次生效；先唤醒 Wait，再按注册顺序在锁外调用所有续接。
    ///
    /// @param result 最终结果（有值 / 无值）。
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

        // 单消费者（一个任务通常只有一个 Get 等待者）：notify_one 即可；
        // 若需多线程等待同一任务，请改回 notify_all。
        m_cv.notify_one();

        // 调试：打印回调触发调用栈与任务标签（仅调试构建有效，发布构建为空操作）。
        TraceInvoke("Complete: 触发续接", Tag());

        for (size_t i = 0; i < vecCbs.size(); ++i)
        {
            if (vecCbs[i])
            {
                vecCbs[i](result);
            }
        }
    }

    /// @brief 注册续接；任务已就绪时投递到执行器异步执行（执行器不可用则视为调用失败）。
    ///
    /// @param pExecutor 执行器句柄（任务永远绑定执行器，恒非空）。
    /// @param fnCallback 续接回调（按值接收，登记时移动存储避免拷贝）。
    /// @return true 续接已登记或已投递；false 任务已就绪但执行器不可用（回调不执行）。
    bool AddContinuation(const std::shared_ptr<CExecutorHandle>& pExecutor, Continuation fnCallback)
    {
        bool bFireNow = false;
        CTaskResult<TValue> result;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_bReady)
            {
                m_vecContinuations.push_back(std::move(fnCallback));
                return true; // 未就绪：已登记，任务完成时触发。
            }
            bFireNow = true;
            result = m_result;
        }

        if (bFireNow && fnCallback)
        {
            // 任务已就绪：投递到执行器线程池异步执行（与 JS/C# 一致）。
            // 执行器未启动/已停止（线程池不可用）→ 视为调用失败，回调不执行。
            if (pExecutor != nullptr && pExecutor->m_pPool != nullptr &&
                !pExecutor->m_bStopped)
            {
                std::function<void()> fnRun = [fnCallback, result]()
                    {
                        fnCallback(result);
                    };
                if (pExecutor->m_pPool->Submit(std::move(fnRun)))
                {
                    // 调试：打印「任务已就绪 → 回调即时投递」调用栈与任务标签。
                    TraceInvoke("AddContinuation: 已就绪回调投递", Tag());
                    return true;
                }
            }
            return false; // 任务已就绪但执行器不可用。
        }
        return true;
    }

    /// @brief 阻塞等待结果。
    ///
    /// @return 最终结果（有值 / 无值）。
    auto Wait() -> CTaskResult<TValue>
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]() { return m_bReady; });
        return m_result;
    }

    /// @brief 设置任务调试标签（任务/续接注册点的函数名）。
    ///
    /// 供调试定位「当前任务回调是哪个函数注册的」；发布构建为空操作。
    ///
    /// @param pszTag 标签（建议传 __PRETTY_FUNCTION__ / NOTHROW_TAG）。
    void SetTag(const char* pszTag)
    {
#if defined(NOTHROW_DEBUG_TRACE)
        m_pszTag = (pszTag != NULL) ? pszTag : "";
#else
        (void)pszTag;
#endif
    }

    /// @brief 获取任务调试标签（发布构建恒为空串）。
    const char* Tag() const
    {
#if defined(NOTHROW_DEBUG_TRACE)
        return m_pszTag;
#else
        return "";
#endif
    }

private:
    std::mutex m_mutex;                  // 保护状态与续接列表。
    std::condition_variable m_cv;        // 通知 Wait 等待者。
    std::vector<Continuation> m_vecContinuations; // 续接列表（未完成时）。
    bool m_bReady;                       // 是否已完成。
    CTaskResult<TValue> m_result;        // 最终结果（完成后有效）。
#if defined(NOTHROW_DEBUG_TRACE)
    const char* m_pszTag;                // 任务/续接注册点（调用方函数名），仅调试。
#endif
};


// ===========================================================================
// ================================= 类型萃取 =================================
// ===========================================================================

/// @brief 变换函数返回类型的特征（分派 RunTransform 用）：
///        Kind = 0 普通值（传播）；1 CTask（flatMap）；2 CTaskResult（原样转发）。
///        ValueType = 解包后的下游结果类型（普通值则原样）。
template <typename T>
struct TaskTraits
{
    static const int Kind = 0;
    using ValueType = T;
};

template <typename U>
struct TaskTraits<CTask<U> >
{
    static const int Kind = 1;
    using ValueType = U;
};

template <typename U>
struct TaskTraits<CTaskResult<U> >
{
    static const int Kind = 2;
    using ValueType = U;
};

/// @brief 变换返回类型的分派标签（TaskTraits::Kind 的 integral_constant）。
template <typename T>
using TaskKind = std::integral_constant<int, TaskTraits<T>::Kind>;

/// @brief 调用结果类型（C++11 兼容，替代已弃用的 std::result_of）。
template <typename TFn, typename... TArgs>
struct TInvokeResult
{
    using type = decltype(std::declval<TFn>()(std::declval<TArgs>()...));
};


// ===========================================================================
// ================================= 辅助函数 =================================
// ===========================================================================

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

/// @brief 执行变换（有参，变换返回普通值 / void）：有值传播，void 完成。
///
/// @param pNextState 下游任务状态。
/// @param f 变换函数。
/// @param valueCopied 上游结果值（const 引用传递，避免按值传参拷贝）。
template <typename TOut, typename TFn, typename TValue>
void RunTransform(const std::shared_ptr<CTaskState<TOut> >& pNextState,
                  TFn f, const TValue& valueCopied, std::integral_constant<int, 0>)
{
    try
    {
        // 复用 CompleteSuccess：返回普通值 → 有值传播；返回 void → 完成。
        CompleteSuccess(pNextState, [f, valueCopied]() { return f(valueCopied); });
    }
    catch (...)
    {
        pNextState->Complete(CTaskResult<TOut>::MakeNone(kException));
    }
}

/// @brief 执行变换（有参，变换返回 CTask）：扁平化 flatMap。
///
/// @param pNextState 下游任务状态。
/// @param f 变换函数。
/// @param valueCopied 上游结果值（const 引用传递，避免按值传参拷贝）。
template <typename TNew, typename TFn, typename TValue>
void RunTransform(const std::shared_ptr<CTaskState<TNew> >& pNextState,
                  TFn f, const TValue& valueCopied, std::integral_constant<int, 1>)
{
    try
    {
        CTask<TNew> inner = f(valueCopied); // 直接初始化（无需默认构造）。
        FlatMapForward(pNextState, inner, typename std::is_same<TNew, void>::type());
    }
    catch (...)
    {
        pNextState->Complete(CTaskResult<TNew>::MakeNone(kException));
    }
}

/// @brief 执行变换（有参，变换返回 CTaskResult）：结果原样转发（Some/None）。
///
/// @param pNextState 下游任务状态。
/// @param f 变换函数。
/// @param valueCopied 上游结果值（const 引用传递，避免按值传参拷贝）。
template <typename TOut, typename TFn, typename TValue>
void RunTransform(const std::shared_ptr<CTaskState<TOut> >& pNextState,
                  TFn f, const TValue& valueCopied, std::integral_constant<int, 2>)
{
    try
    {
        pNextState->Complete(f(valueCopied)); // 原样转发（有值/无值）。
    }
    catch (...)
    {
        pNextState->Complete(CTaskResult<TOut>::MakeNone(kException));
    }
}

/// @brief 执行变换（无参，void 上游的 Then 用）：变换返回普通值 / void。
template <typename TOut, typename TFn>
void RunTransformVoid(const std::shared_ptr<CTaskState<TOut> >& pNextState,
                      TFn f, std::integral_constant<int, 0>)
{
    try
    {
        // 复用 CompleteSuccess（f 无参）：返回普通值 → 有值传播；void → 完成。
        CompleteSuccess(pNextState, f);
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
    try
    {
        CTask<TNew> inner = f(); // 直接初始化（无需默认构造）。
        FlatMapForward(pNextState, inner, typename std::is_same<TNew, void>::type());
    }
    catch (...)
    {
        pNextState->Complete(CTaskResult<TNew>::MakeNone(kException));
    }
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
    catch (...)
    {
        pNextState->Complete(CTaskResult<TOut>::MakeNone(kException));
    }
}
} // namespace detail

/// @brief 任务公共基类（提取 CTask 与 CTask<void> 共享的句柄、状态与 Get）。
template <typename TValue>
class CTaskBase
{
public:
    /// @brief 阻塞获取最终结果（不抛异常）。
    ///
    /// @return 最终结果（有值 / 无值）。
    auto Get() const -> CTaskResult<TValue> { return m_pState->Wait(); }

protected:
    /// 创建空任务（仅供派生类构造）。
    CTaskBase()
        : m_pExecutor(),
          m_pState(std::make_shared<detail::CTaskState<TValue> >())
    {
    }

    template <typename U> friend class CTask; // 各 CTask<U> 互访（Then 跨实例）。
    friend class CAsyncExecutor;              // Submit 注入执行器句柄。

    std::shared_ptr<detail::CExecutorHandle> m_pExecutor;   // 执行器句柄（续接投递用）。
    std::shared_ptr<detail::CTaskState<TValue>> m_pState;  // 任务共享状态。
};

/// @brief 异步任务（Option 风格），支持链式调用（Then）。
///
/// 由 CAsyncExecutor::Submit 创建；
/// 通过 Then 串联后续步骤，Get 阻塞获取最终结果（CTaskResult，不抛异常）。
/// 上游无值（终止）时 Then 不再执行，终止原因传播给下游。
/// 句柄、状态与 Get 由基类 CTaskBase<TValue> 提供。任务永远绑定一个执行器。
///
/// @tparam TValue 任务携带的值类型。
template <typename TValue>
class CTask : public CTaskBase<TValue>
{
public:
    /// @brief 链式续接：上游有值时执行 fnTransform(value)，上游无值则终止传播。
    ///
    /// 变换函数返回三种：
    ///  - 普通值 TNew：有值传播（Then → Then → Get）；
    ///  - `CTaskResult<TNew>`：有值传播 / 无值（None）终止；
    ///  - `CTask<TNew>`：扁平化（flatMap），内部任务完成后转发其结果。
    ///
    /// @param fnTransform 变换函数。
    /// @param szTag 下游任务调试标签（可选，建议传 NOTHROW_TAG 定位注册点）。
    /// @return 下游任务（延续链）。
    template <typename TFn>
    auto Then(TFn fnTransform, const char* szTag = NULL)
        -> CTask<typename detail::TaskTraits<
            typename detail::TInvokeResult<TFn, TValue>::type>::ValueType>;

    /// 回调类型别名。
    using SuccessCallback = std::function<void(const TValue&)>;        // 有值回调。
    using NoneCallback = std::function<void(detail::CTaskEndReason)>;  // 无值回调。

    /// @brief 注册成功回调（有值时触发）。
    ///
    /// @param fnCallback 有值回调。
    /// @return true 注册/投递成功；false 任务已就绪但执行器不可用（回调不执行）。
    bool OnSuccess(SuccessCallback fnCallback);

    /// @brief 注册无值回调（链终止时触发；参数为终止原因，调试用）。
    ///
    /// @param fnCallback 无值回调。
    /// @return true 注册/投递成功；false 任务已就绪但执行器不可用（回调不执行）。
    bool OnNone(NoneCallback fnCallback);

private:
    /// 创建空任务（仅供框架内部：Then/flatMap/Submit 构造下游）。
    /// @warning 空任务永不完成，直接 Get() 将永久阻塞。
    CTask() {}

    template <typename U> friend class CTask; // 各 CTask<U> 互访（Then 构造下游）。
    friend class CAsyncExecutor;              // Submit 构造任务。
};

/// @brief OnSuccess 实现（非 void）。
template <typename TValue>
bool CTask<TValue>::OnSuccess(SuccessCallback fnCallback)
{
    return this->m_pState->AddContinuation(this->m_pExecutor,
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
bool CTask<TValue>::OnNone(NoneCallback fnCallback)
{
    return this->m_pState->AddContinuation(this->m_pExecutor,
        [fnCallback](const CTaskResult<TValue>& result)
        {
            if (!result.HasValue() && fnCallback)
            {
                fnCallback(result.Reason());
            }
        });
}

/// @brief Then 实现（非 void 上游）：注册上游续接，有值则投递变换，无值则传播终止。
template <typename TValue>
template <typename TFn>
auto CTask<TValue>::Then(TFn f, const char* szTag /* = NULL */)
    -> CTask<typename detail::TaskTraits<
        typename detail::TInvokeResult<TFn, TValue>::type>::ValueType>
{
    using TResult = typename detail::TInvokeResult<TFn, TValue>::type; // 变换原始返回类型。
    using TOut = typename detail::TaskTraits<TResult>::ValueType;      // 解包后下游结果类型。
    CTask<TOut> taskNext;
    taskNext.m_pExecutor = this->m_pExecutor; // 沿用上游执行器句柄（续接投递用）。
    taskNext.m_pState->SetTag(szTag);         // 记录下游任务调试标签（发布构建为空操作）。
    auto pNextState = taskNext.m_pState;
    auto pExecutor = this->m_pExecutor;

    // 注册上游续接：未就绪 → 登记（任务完成时触发）；已就绪 → 投递到执行器异步执行。
    // 返回 false：任务已就绪但执行器不可用，续接无法投递，下游立即以失败（kStopped）完成。
    bool bOk = this->m_pState->AddContinuation(pExecutor,
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
                    detail::RunTransform(pNextState, f, valueCopied, detail::TaskKind<TResult>());
                };

            // ④ 在执行器上执行；执行器不可用（未启动/已停止）→ 视为失败（kStopped）。
            if (pExecutor == nullptr || pExecutor->m_pPool == nullptr ||
                pExecutor->m_bStopped || !pExecutor->m_pPool->Submit(std::move(fnRun)))
            {
                pNextState->Complete(CTaskResult<TOut>::MakeNone(detail::kStopped));
            }
        });

    if (!bOk)
    {
        pNextState->Complete(CTaskResult<TOut>::MakeNone(detail::kStopped));
    }
    return taskNext;
}


/// @brief 异步任务（Option 风格，TValue 为 void 的特化）。
///
/// 用于无返回值任务：也支持 Then（变换函数无参数，可继续链）。
/// OnSuccess 回调无参数。
template <>
class CTask<void> : public CTaskBase<void>
{
public:
    /// @brief 链式续接：上游完成时执行 fnTransform()（无参数），上游终止则传播。
    ///
    /// @param fnTransform 变换函数（无参数）。
    /// @param szTag 下游任务调试标签（可选，建议传 NOTHROW_TAG 定位注册点）。
    /// @return 下游任务（延续链）。
    template <typename TFn>
    auto Then(TFn fnTransform, const char* szTag = NULL)
        -> CTask<typename detail::TaskTraits<
            typename detail::TInvokeResult<TFn>::type>::ValueType>;

    /// 回调类型别名。
    using SuccessCallback = std::function<void()>;                      // 完成回调。
    using NoneCallback = std::function<void(detail::CTaskEndReason)>;  // 无值回调。

    /// @brief 注册完成回调（无参数）。
    ///
    /// @param fnCallback 完成回调。
    /// @return true 注册/投递成功；false 任务已就绪但执行器不可用（回调不执行）。
    bool OnSuccess(SuccessCallback fnCallback);

    /// @brief 注册无值回调（链终止时触发；参数为终止原因，调试用）。
    ///
    /// @param fnCallback 无值回调。
    /// @return true 注册/投递成功；false 任务已就绪但执行器不可用（回调不执行）。
    bool OnNone(NoneCallback fnCallback);

private:
    /// 创建空任务（仅供框架内部：Then/flatMap/Submit 构造下游）。
    /// @warning 空任务永不完成，直接 Get() 将永久阻塞。
    CTask() {}

    template <typename U> friend class CTask; // 各 CTask<U> 互访（Then 构造下游）。
    friend class CAsyncExecutor;              // Submit 构造任务。
};

/// @brief Then 实现（void 上游）：上游完成则执行 fn()（无参），终止则传播。
template <typename TFn>
auto CTask<void>::Then(TFn f, const char* szTag /* = NULL */)
    -> CTask<typename detail::TaskTraits<
        typename detail::TInvokeResult<TFn>::type>::ValueType>
{
    using TResult = typename detail::TInvokeResult<TFn>::type;
    using TOut = typename detail::TaskTraits<TResult>::ValueType;
    CTask<TOut> taskNext;
    taskNext.m_pExecutor = this->m_pExecutor;
    taskNext.m_pState->SetTag(szTag); // 记录下游任务调试标签（发布构建为空操作）。
    auto pNextState = taskNext.m_pState;
    auto pExecutor = this->m_pExecutor;

    // 注册上游续接：未就绪 → 登记（任务完成时触发）；已就绪 → 投递到执行器异步执行。
    // 返回 false：任务已就绪但执行器不可用，续接无法投递，下游立即以失败（kStopped）完成。
    bool bOk = this->m_pState->AddContinuation(pExecutor,
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
                    detail::RunTransformVoid(pNextState, f, detail::TaskKind<TResult>());
                };

            // ③ 在执行器上执行；执行器不可用（未启动/已停止）→ 视为失败（kStopped）。
            if (pExecutor == nullptr || pExecutor->m_pPool == nullptr ||
                pExecutor->m_bStopped || !pExecutor->m_pPool->Submit(std::move(fnRun)))
            {
                pNextState->Complete(CTaskResult<TOut>::MakeNone(detail::kStopped));
            }
        });
    if (!bOk)
    {
        pNextState->Complete(CTaskResult<TOut>::MakeNone(detail::kStopped));
    }
    return taskNext;
}







/// @brief 基于线程池执行任务，支持链式调用（Submit → Then → Get）。
/// 无模板参数（错误不再是框架概念）。
///
/// @note 生命周期：任务链通过共享句柄引用执行器线程池；执行器析构后，
///       已投递/已链式任务仍安全完成，新投递以无值（kStopped）完成。
class CAsyncExecutor
{
public:
    /// @brief 创建执行器（指定工作线程数，默认 1）。
    ///
    /// @param nThreadCount 工作线程数。
    explicit CAsyncExecutor(size_t nThreadCount = 1);

    /// @brief 不可拷贝（拷贝会共享线程池，Stop 相互影响）。
    CAsyncExecutor(const CAsyncExecutor&) = delete;
    CAsyncExecutor& operator=(const CAsyncExecutor&) = delete;

    /// @brief 销毁执行器（停止并等待任务完成）。
    ~CAsyncExecutor();

    /// @brief 启动工作线程。
    ///
    /// @return true 启动成功；false 已启动或线程数为 0。
    bool Start();

    /// @brief 提交任务并返回 CTask（任务内部异常自动转为无值终止）。
    ///
    /// @tparam TFn 任务函数类型（返回值作为任务结果）。
    /// @param f 任务函数（在工作线程上执行）。
    /// @param szTag 任务调试标签（可选，建议传 NOTHROW_TAG 定位注册点）。
    /// @return 关联本执行器的任务；执行器未启动时任务立即以无值（kNotStarted）完成。
    template <typename TFn>
    auto Submit(TFn f, const char* szTag = NULL) -> CTask<typename detail::TInvokeResult<TFn>::type>;

    /// 无返回值任务类型（fire-and-forget）。
    using TaskCallback = std::function<void()>;

    /// @brief 提交无返回值任务（fire-and-forget；按值接收，移动投递避免拷贝）。
    ///
    /// @param fnTask 任务函数。
    /// @return true 提交成功；false 执行器未启动。
    bool Post(TaskCallback fnTask);

    /// @brief 停止并等待任务完成（优雅关闭）。
    void Stop();

    /// @brief 是否正在运行。
    bool IsRunning() const;

private:
    std::shared_ptr<detail::CExecutorHandle> m_pHandle; // 执行器句柄（任务链共享）。
    size_t m_nThreadCount;                              // 工作线程数。
};

/// @brief Submit 实现：把任务函数投递到线程池执行。
template <typename TFn>
auto CAsyncExecutor::Submit(TFn f, const char* szTag /* = NULL */)
    -> CTask<typename detail::TInvokeResult<TFn>::type>
{
    using TResult = typename detail::TInvokeResult<TFn>::type; // 任务结果类型。
    CTask<TResult> task;
    task.m_pExecutor = m_pHandle; // 共享执行器句柄（任务链持有时线程池不释放）。
    task.m_pState->SetTag(szTag); // 记录任务调试标签（发布构建为空操作）。
    auto pState = task.m_pState;
    auto pHandle = m_pHandle;
    std::function<void()> fnRun = [pState, f]()
        {
            try
            {
                // 执行任务；成功完成（有值或 void 完成）。f 可能抛异常：捕获并转为无值终止。
                detail::CompleteSuccess(pState, f);
            }
            catch (...)
            {
                pState->Complete(CTaskResult<TResult>::MakeNone(detail::kException));
            }
        };
    if (pHandle->m_bStopped || !pHandle->m_pPool->Submit(std::move(fnRun)))
    {
        // 执行器不可用（未启动/已停止）：任务立即以无值完成。
        pState->Complete(CTaskResult<TResult>::MakeNone(detail::kNotStarted));
    }
    return task;
}

} // namespace nothrow
} // namespace common
