#pragma once

#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "Thread/ThreadPool.h"

// ====================================================================
// 无异常版异步框架（错误码 + CTaskResult<TValue>，风格类似 std::expected<T, Error>）
//
// 与 Common/Async/AsyncExecutor.h（异常版）的区别：
//  - 不使用 std::exception_ptr、不抛异常；错误统一用「错误码 + 消息」传递；
//  - CTask::Get() 返回 CTaskResult<TValue>（不抛异常，需检查 Ok()/Failed()）；
//  - 任务内部抛出的异常被捕获并转为 kTaskFailed 错误码，不向外传播；
//  - 调用方无需 try/catch：错误一律通过返回值传递。
//
// 「无异常」的准确含义（重要）：
//  - 对外契约：本框架 API（Submit/Then/Get/OnSuccess/OnFailure）不向调用方抛异常，
//    错误通过 CTaskResult / CTaskError 传递，调用方无需 try/catch；
//  - 内部实现：任务函数（Submit/Then 的入参 f）是调用方提供的任意代码，无法保证不 throw。
//    框架用 try/catch 把用户异常捕获并转为 kTaskFailed 错误码，防止异常逃逸到工作线程
//    导致 std::terminate 崩溃，从而维持对外「无异常」契约；
//  - 因此本文件包含 try/catch，不兼容 -fno-exceptions 编译；若需彻底 -fno-exceptions，
//    应约定任务函数禁止 throw 并移除内部 try/catch（此时错误仍走错误码通道）。
//
// 用法示例：
// @code
//   common::nothrow::CAsyncExecutor exec(2);
//   exec.Start();
//
//   // 链式调用：Submit → Then → Then，Get 返回结果（不抛异常）
//   common::nothrow::CTaskResult<int> r =
//       exec.Submit([]() { return 3; })
//           .Then([](int n) { return n * 2; })
//           .Then([](int n) { return n + 1; })
//           .Get();
//   if (r.Ok())     { /* 成功，取值 r.Value() */ }
//   if (r.Failed()) { /* 失败，取错误 r.Error().nCode / r.Error().strMessage */ }
// @endcode
//
// 特性：
//  - 链式 Then 支持同步变换与「扁平化」：变换函数可返回普通值，也可返回 CTask<TNew>，
//    后者自动平铺为下游任务（类似 Promise.then 的 flatMap 语义，错误沿链传播）；
//  - 执行器生命周期加固：任务链通过共享句柄引用执行器线程池；执行器析构后，
//    已投递/已链式任务仍安全完成（无悬垂指针），新投递返回 kExecutorStopped；
//  - 线程模型：任务函数与续接在工作线程执行；若任务已完成，OnSuccess/OnFailure/Then
//    注册的回调在注册线程上同步触发，调用方不得假设回调固定在某一线程。
// ====================================================================

namespace common {
namespace nothrow {

/// @brief 任务错误码（无异常的错误通道）。
///
/// 框架内置的错误类别；业务可自定义错误码（见 CTaskError::nCode 说明）。
/// 任务失败时通过错误码标识失败类别，配合 CTaskError::strMessage 携带具体原因。
enum TaskErrorCode
{
    kTaskOk = 0,         // 成功（无错误）。
    kTaskFailed,         // 任务执行抛出异常（已捕获并转为错误）。
    kExecutorNotStarted, // 执行器未启动（Submit 时线程池不存在/未运行）。
    kExecutorStopped     // 执行器已停止（Then 投递时线程池已关闭）。
};

/// @brief 任务错误信息（无异常的错误通道）。
///
/// 由「错误码 + 可读消息」组成：错误码见 TaskErrorCode 枚举，
/// 消息携带具体原因（如任务抛出的异常 what() 文本）。
///
/// @note nCode 用 int 而非 TaskErrorCode：TaskErrorCode 只是框架内置的
///       建议错误码集合；用 int 允许业务方传入自定义错误码（如 1000+ 业务码），
///       便于扩展。TaskErrorCode 枚举值可隐式转 int，直接传 kTaskFailed 等亦可。
struct CTaskError
{
    int nCode;            // 错误码（TaskErrorCode 内置值或业务自定义值）。
    std::string strMessage; // 错误描述（可读原因）。

    /// 默认构造：kTaskOk（成功、无错误）。
    CTaskError() : nCode(kTaskOk) {}

    /// 构造指定错误码与消息。
    ///
    /// @param nErrorCode 错误码（TaskErrorCode 内置值或业务自定义值）。
    /// @param strError   错误描述。
    CTaskError(int nErrorCode, const std::string& strError)
        : nCode(nErrorCode), strMessage(strError) {}

    /// 是否失败（错误码非 kTaskOk）。
    bool Failed() const { return nCode != kTaskOk; }
};

// 模板前向声明（默认模板实参引用 CTaskError，故置于 CTaskError 定义之后）。
template <typename TError = CTaskError> class CAsyncExecutor;
template <typename TValue, typename TError = CTaskError> class CTask;

namespace detail {

/// @brief 生成默认错误值：CTaskError 默认「未初始化失败」，其它错误类型用其默认构造。
template <typename TError>
TError MakeDefaultError()
{
    return TError();
}

template <>
inline CTaskError MakeDefaultError<CTaskError>()
{
    return CTaskError(kTaskFailed, "uninitialized");
}

/// @brief 由错误码 + 文本生成错误值：错误类型可用「错误码 + 消息」构造时（如 CTaskError）
///        生成对应错误；否则回退到默认错误。
template <typename TError>
TError MakeErrorFromCode(int nCode, const std::string& strMessage, std::true_type)
{
    return TError(nCode, strMessage);
}

template <typename TError>
TError MakeErrorFromCode(int, const std::string&, std::false_type)
{
    return MakeDefaultError<TError>();
}

template <typename TError>
TError MakeErrorFromCode(int nCode, const std::string& strMessage)
{
    return MakeErrorFromCode<TError>(nCode, strMessage,
        typename std::is_constructible<TError, int, const std::string&>::type());
}

} // namespace detail

/// @brief 任务结果（类似 std::expected&lt;T, E&gt;）。
///
/// 一个结果要么「成功携带值」、要么「失败携带错误」，二者互斥：
///  - Ok()     为 true 时，可经 Value() 取成功值；
///  - Failed() 为 true 时，可经 Error() 取错误信息。
/// 通过静态工厂 Success / Failure 创建。
///
/// @tparam TValue 成功时携带的值类型。
/// @tparam TError 错误类型（默认 CTaskError；可自定义，类似 std::expected&lt;T, E&gt;）。
template <typename TValue, typename TError = CTaskError>
class CTaskResult
{
public:
    /// 创建成功结果（拷贝 value）。
    ///
    /// @param value 成功值（拷贝存入结果）。
    static CTaskResult Success(const TValue& value)
    {
        CTaskResult r;
        r.m_pValue.reset(new TValue(value));
        return r;
    }

    /// 创建成功结果（移动 value，支持 move-only 值类型 / 减少一次拷贝）。
    ///
    /// @param value 成功值（移动存入结果）。
    static CTaskResult Success(TValue&& value)
    {
        CTaskResult r;
        r.m_pValue.reset(new TValue(std::move(value)));
        return r;
    }

    /// 创建失败结果（携带任意错误类型，类似 std::unexpected&lt;E&gt;）。
    ///
    /// @param error 错误值。
    static CTaskResult Failure(const TError& error)
    {
        CTaskResult r;
        r.m_error = error;
        return r;
    }

    /// 是否成功（持有值）。
    bool Ok() const { return m_pValue != nullptr; }

    /// 是否失败（未持有值）。
    bool Failed() const { return m_pValue == nullptr; }

    /// 是否成功（便捷写法：if (result)）。
    explicit operator bool() const { return Ok(); }

    /// 成功时的值（仅在 Ok() 为 true 时调用）。
    const TValue& Value() const { return *m_pValue; }

    /// 成功时返回值，失败时返回 defValue。
    ///
    /// @param defValue 失败时的默认值。
    TValue ValueOr(const TValue& defValue) const
    {
        return Ok() ? *m_pValue : defValue;
    }

    /// 失败时的错误（仅在 Failed() 为 true 时调用）。
    const TError& Error() const { return m_error; }

    /// 空结果（默认失败态：无值，错误为默认错误）；通常用 Success / Failure 工厂创建。
    CTaskResult() : m_error(detail::MakeDefaultError<TError>()) {}

private:
    std::shared_ptr<TValue> m_pValue; // 成功值（非空 ⇔ 成功）。
    TError m_error;                   // 错误信息（失败时有效）。
};

/// @brief CTaskResult&lt;void, TError&gt; 特化（成功无值，仅携带成败与错误）。
///
/// 用于无返回值的任务：成功时无值可取，仅 Ok() 为 true；失败时同主模板。
///
/// @tparam TError 错误类型（默认 CTaskError）。
template <typename TError>
class CTaskResult<void, TError>
{
public:
    /// 创建成功结果（无值）。
    static CTaskResult Success()
    {
        CTaskResult r;
        r.m_bOk = true;
        return r;
    }

    /// 创建失败结果（携带任意错误类型）。
    ///
    /// @param error 错误值。
    static CTaskResult Failure(const TError& error)
    {
        CTaskResult r;
        r.m_bOk = false;
        r.m_error = error;
        return r;
    }

    /// 是否成功。
    bool Ok() const { return m_bOk; }

    /// 是否失败。
    bool Failed() const { return !m_bOk; }

    /// 失败时的错误（仅在 Failed() 为 true 时调用）。
    const TError& Error() const { return m_error; }

    /// 空结果（默认失败态：无值，错误为默认错误）；通常用 Success / Failure 工厂创建。
    CTaskResult() : m_bOk(false), m_error(detail::MakeDefaultError<TError>()) {}

private:
    bool m_bOk;   // 是否成功。
    TError m_error; // 错误信息（失败时有效）。
};

namespace detail {

/// @brief 任务共享状态（无异常版）：结果 + 续接列表 + 同步等待。
///
/// 负责管理一个任务的最终结果（CTaskResult&lt;TValue, TError&gt;）、已注册的续接列表，
/// 以及阻塞等待（Wait）的同步原语。线程安全：
///  - 状态变更（Complete / AddContinuation）由 m_mutex 保护；
///  - 续接在锁外调用，避免回调内重入本状态时死锁；
///  - Wait 用 m_cv 等待就绪。
///
/// @tparam TValue 任务结果携带的值类型。
/// @tparam TError 错误类型（默认 CTaskError）。
template <typename TValue, typename TError = CTaskError>
class CTaskState
{
public:
    /// 续接回调：接收最终结果（成功值或错误）。
    using Continuation = std::function<void(const CTaskResult<TValue, TError>&)>;

    /// 创建状态（初始未就绪）。
    CTaskState() : m_bReady(false) {}

    /// 完成并触发续接（锁外调用，防重入死锁）。
    ///
    /// 仅首次生效（重复 Complete 被忽略）；先唤醒 Wait，再按注册顺序
    /// 在锁外调用所有续接。
    ///
    /// @param result 最终结果（成功或失败）。
    void Complete(const CTaskResult<TValue, TError>& result)
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
    ///
    /// 任务未完成时入队；已完成时立即在调用线程执行回调。
    ///
    /// @param fnCallback 续接回调。
    void AddContinuation(const Continuation& fnCallback)
    {
        bool bFireNow = false;
        CTaskResult<TValue, TError> result;
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

    /// 阻塞等待结果（返回 CTaskResult，不抛异常）。
    ///
    /// 阻塞调用线程直到任务完成；返回最终结果，调用方检查 Ok()/Failed()。
    auto Wait() -> CTaskResult<TValue, TError>
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]() { return m_bReady; });
        return m_result;
    }

private:
    std::mutex m_mutex;                 // 保护状态与续接列表。
    std::condition_variable m_cv;       // 通知 Wait 等待者。
    std::vector<Continuation> m_vecContinuations; // 续接列表（未完成时）。
    bool m_bReady;                      // 是否已完成。
    CTaskResult<TValue, TError> m_result; // 最终结果（完成后有效）。
};

/// @brief 以「成功」完成状态（结果类型非 void，携带 f() 的返回值）。
///
/// @tparam TResult 结果值类型（非 void）。
/// @tparam TError  错误类型。
/// @tparam TFn     任务函数类型。
template <typename TResult, typename TError, typename TFn>
void CompleteSuccess(const std::shared_ptr<CTaskState<TResult, TError> >& pState, TFn f)
{
    pState->Complete(CTaskResult<TResult, TError>::Success(f()));
}

/// @brief 以「成功」完成状态（结果类型为 void，执行 f 但不携带值）。
///
/// @tparam TError 错误类型。
/// @tparam TFn    任务函数类型。
template <typename TError, typename TFn>
void CompleteSuccess(const std::shared_ptr<CTaskState<void, TError> >& pState, TFn f)
{
    f();
    pState->Complete(CTaskResult<void, TError>::Success());
}

/// @brief 调用结果类型（C++11 兼容，替代已弃用的 std::result_of）。
///
/// @tparam TFn   可调用对象类型。
/// @tparam TArgs 参数类型（可为空）。
template <typename TFn, typename... TArgs>
struct TInvokeResult
{
    using type = decltype(std::declval<TFn>()(std::declval<TArgs>()...));
};

/// @brief 是否为 CTask&lt;U, E&gt;（用于识别「变换函数返回任务」）。
template <typename T>
struct IsTask : std::false_type
{
};

template <typename U, typename E>
struct IsTask<CTask<U, E> > : std::true_type
{
};

/// @brief 解包任务类型：CTask&lt;U, E&gt; -&gt; U；其它 T -&gt; T。
///
/// 用于 Then 的返回类型：若变换函数返回 CTask&lt;TNew&gt;，则下游任务携带 TNew。
template <typename T>
struct UnwrapTask
{
    using type = T;
};

template <typename U, typename E>
struct UnwrapTask<CTask<U, E> >
{
    using type = U;
};

/// @brief 执行器句柄（生命周期加固核心）。
///
/// 任务链通过 shared_ptr 持有句柄，从而共享底层线程池：
///  - 执行器对象析构后，句柄仍被任务链持有，线程池不释放 → 无悬垂指针；
///  - m_bStopped 置位后不再接受新投递，已投递任务继续执行完毕。
struct CExecutorHandle
{
    std::shared_ptr<common::CThreadPool> m_pPool; // 工作线程池（任务链持有时不释放）。
    std::atomic<bool> m_bStopped;                 // 是否已停止（停止后拒绝新投递）。

    CExecutorHandle() : m_bStopped(false) {}
};

} // namespace detail

/// @brief 异步任务（无异常版），支持链式调用（Then）。
///
/// 由 CAsyncExecutor::Submit / CTask::FromResult 创建；
/// 通过 Then 串联后续步骤，Get 阻塞获取最终结果（CTaskResult，不抛异常）。
/// 错误以 CTaskResult 沿链传播：上游失败时 Then 不再执行，错误传递给下游。
///
/// @tparam TValue 任务携带的值类型。
/// @tparam TError 错误类型（默认 CTaskError，沿链传播）。
template <typename TValue, typename TError>
class CTask
{
public:
    /// 创建空任务（未完成）。
    CTask()
        : m_pExecutor(),
          m_pState(std::make_shared<detail::CTaskState<TValue, TError> >())
    {
    }

    /// 从已就绪结果创建任务（可启动链式调用）。
    ///
    /// @param result 已就绪的结果（成功或失败）。
    static auto FromResult(const CTaskResult<TValue, TError>& result)
        -> CTask<TValue, TError>
    {
        CTask<TValue, TError> task;
        task.m_pState->Complete(result);
        return task;
    }

    /// 链式续接：上游成功后在执行器上运行 fnTransform(value)，
    /// 上游失败则错误传播给下游（fnTransform 不执行）。
    ///
    /// 支持两种变换函数：
    ///  - 返回普通值 TNew：直接作为下游结果（Then → Then → Get）；
    ///  - 返回 CTask&lt;TNew, TError&gt;：自动扁平化（flatMap），内部任务完成后
    ///    将其结果转发为下游结果，内部任务失败则错误沿链传播。
    ///
    /// @tparam TFn 变换函数类型（接收 TValue，返回 TNew 或 CTask&lt;TNew&gt;）。
    /// @param fnTransform 变换函数。
    /// @return 下游任务（携带变换后的结果类型）。
    template <typename TFn>
    auto Then(TFn fnTransform)
        -> CTask<typename detail::UnwrapTask<
            typename detail::TInvokeResult<TFn, TValue>::type>::type, TError>;

    /// 阻塞获取最终结果（不抛异常）。
    ///
    /// 调用线程阻塞至任务完成；返回 CTaskResult，需检查 Ok()/Failed()。
    auto Get() const -> CTaskResult<TValue, TError> { return m_pState->Wait(); }

    /// 注册成功回调（fire-and-forget，任务成功时触发）。
    ///
    /// @param fnCallback 成功回调（参数为成功值）。
    void OnSuccess(const std::function<void(const TValue&)>& fnCallback);

    /// 注册失败回调（fire-and-forget，任务失败时触发）。
    ///
    /// @param fnCallback 失败回调（参数为错误信息）。
    void OnFailure(const std::function<void(const TError&)>& fnCallback);

private:
    template <typename UError> friend class CAsyncExecutor;
    template <typename UValue, typename UError> friend class CTask;

    std::shared_ptr<detail::CExecutorHandle> m_pExecutor; // 执行器句柄（续接投递用，可空）。
    std::shared_ptr<detail::CTaskState<TValue, TError> > m_pState; // 任务共享状态。
};

/// @brief 异步任务（无异常版，TValue 为 void 的特化）。
///
/// 用于无返回值任务：OnSuccess 回调无参数；不支持 Then（void 任务无后续值可变换）。
///
/// @tparam TError 错误类型（默认 CTaskError）。
template <typename TError>
class CTask<void, TError>
{
public:
    /// 创建空任务（未完成）。
    CTask()
        : m_pExecutor(),
          m_pState(std::make_shared<detail::CTaskState<void, TError> >())
    {
    }

    /// 从已就绪结果创建任务。
    ///
    /// @param result 已就绪的结果（成功或失败）。
    static auto FromResult(const CTaskResult<void, TError>& result)
        -> CTask<void, TError>
    {
        CTask<void, TError> task;
        task.m_pState->Complete(result);
        return task;
    }

    /// 阻塞获取最终结果（不抛异常）。
    ///
    /// 调用线程阻塞至任务完成；返回 CTaskResult，需检查 Ok()/Failed()。
    auto Get() const -> CTaskResult<void, TError> { return m_pState->Wait(); }

    /// 注册成功回调（fire-and-forget，任务成功时触发；无值参数）。
    ///
    /// @param fnCallback 成功回调（无参数）。
    void OnSuccess(const std::function<void()>& fnCallback);

    /// 注册失败回调（fire-and-forget，任务失败时触发）。
    ///
    /// @param fnCallback 失败回调（参数为错误信息）。
    void OnFailure(const std::function<void(const TError&)>& fnCallback);

private:
    template <typename UError> friend class CAsyncExecutor;
    template <typename UValue, typename UError> friend class CTask;

    std::shared_ptr<detail::CExecutorHandle> m_pExecutor; // 执行器句柄（续接投递用，可空）。
    std::shared_ptr<detail::CTaskState<void, TError> > m_pState;  // 任务共享状态。
};

/// @brief 异步执行器（无异常版）。
///
/// 基于线程池执行任务，支持链式调用（Submit → Then → Get）。
/// 错误类型可自定义：Submit 产生的任务链统一携带 TError（默认 CTaskError）。
///
/// @tparam TError 错误类型（默认 CTaskError）。
///
/// @note 生命周期：任务链通过共享句柄引用执行器线程池；执行器析构后，
///       已投递/已链式任务仍安全完成（无悬垂指针），新投递返回 kExecutorStopped。
template <typename TError>
class CAsyncExecutor
{
public:
    /// 创建执行器（指定工作线程数）。
    ///
    /// @param nThreadCount 工作线程数量（默认 1）。
    explicit CAsyncExecutor(size_t nThreadCount = 1);

    /// 销毁执行器（停止并等待任务完成）。
    ~CAsyncExecutor();

    /// 启动工作线程。
    ///
    /// @return true 启动成功；false 已启动或线程数为 0。
    bool Start();

    /// 提交任务并返回 CTask（任务内部异常自动转为 TError 错误）。
    ///
    /// @tparam TFn 任务函数类型（返回新任务的结果类型）。
    /// @param f 任务函数（在工作线程上执行）。
    /// @return 关联本执行器的任务；执行器未启动时任务立即以
    ///         kExecutorNotStarted 错误完成。
    template <typename TFn>
    auto Submit(TFn f)
        -> CTask<typename detail::TInvokeResult<TFn>::type, TError>;

    /// 提交无返回值任务（fire-and-forget）。
    ///
    /// @param fnTask 无返回值任务。
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

namespace detail {

/// @brief 转发内部任务结果（TNew 非 void）：成功带值 / 失败带错误。
///
/// @tparam TNew   内部任务的结果值类型（非 void）。
/// @tparam TError 错误类型。
template <typename TNew, typename TError>
void FlatMapForward(const std::shared_ptr<CTaskState<TNew, TError> >& pNextState,
                    CTask<TNew, TError>& inner, std::false_type)
{
    inner.OnSuccess([pNextState](const TNew& value)
    {
        pNextState->Complete(CTaskResult<TNew, TError>::Success(value));
    });
    inner.OnFailure([pNextState](const TError& e)
    {
        pNextState->Complete(CTaskResult<TNew, TError>::Failure(e));
    });
}

/// @brief 转发内部任务结果（TNew 为 void）：成功无值 / 失败带错误。
///
/// @tparam TNew   内部任务的结果值类型（void）。
/// @tparam TError 错误类型。
template <typename TNew, typename TError>
void FlatMapForward(const std::shared_ptr<CTaskState<TNew, TError> >& pNextState,
                    CTask<TNew, TError>& inner, std::true_type)
{
    inner.OnSuccess([pNextState]()
    {
        pNextState->Complete(CTaskResult<TNew, TError>::Success());
    });
    inner.OnFailure([pNextState](const TError& e)
    {
        pNextState->Complete(CTaskResult<TNew, TError>::Failure(e));
    });
}

/// @brief 执行变换（变换函数返回普通值）：成功则完成下游（值或 void）。
///
/// f 是调用方任意代码，可能抛异常：捕获并转为错误码，防止逃逸到工作线程。
///
/// @tparam TOut   下游结果值类型（非 void）。
/// @tparam TError 错误类型。
/// @tparam TFn    变换函数类型。
/// @tparam TValue 上游值类型。
template <typename TOut, typename TError, typename TFn, typename TValue>
void RunTransform(const std::shared_ptr<CTaskState<TOut, TError> >& pNextState,
                  TFn f, TValue valueCopied, std::false_type)
{
    try
    {
        CompleteSuccess(pNextState,
            [valueCopied, f]() { return f(valueCopied); });
    }
    catch (const std::exception& e)
    {
        pNextState->Complete(CTaskResult<TOut, TError>::Failure(
            MakeErrorFromCode<TError>(kTaskFailed, e.what())));
    }
    catch (...)
    {
        pNextState->Complete(CTaskResult<TOut, TError>::Failure(
            MakeErrorFromCode<TError>(kTaskFailed, "unknown error")));
    }
}

/// @brief 执行变换（变换函数返回 CTask&lt;TNew&gt;）：扁平化 flatMap。
///
/// f 返回内部任务；内部任务完成后将其结果转发为下游结果（成功或失败），
/// 从而把「返回任务的变换」自动平铺进当前链（目标 CTask&lt;void&gt; 亦支持）。
///
/// @tparam TNew   内部任务的结果值类型（可为 void）。
/// @tparam TError 错误类型。
/// @tparam TFn    变换函数类型。
/// @tparam TValue 上游值类型。
template <typename TNew, typename TError, typename TFn, typename TValue>
void RunTransform(const std::shared_ptr<CTaskState<TNew, TError> >& pNextState,
                  TFn f, TValue valueCopied, std::true_type)
{
    CTask<TNew, TError> inner;
    try
    {
        inner = f(valueCopied);
    }
    catch (const std::exception& e)
    {
        pNextState->Complete(CTaskResult<TNew, TError>::Failure(
            MakeErrorFromCode<TError>(kTaskFailed, e.what())));
        return;
    }
    catch (...)
    {
        pNextState->Complete(CTaskResult<TNew, TError>::Failure(
            MakeErrorFromCode<TError>(kTaskFailed, "unknown error")));
        return;
    }
    // 内部任务完成后，转发其结果（成功或失败）为下游结果（支持目标为 void）。
    FlatMapForward(pNextState, inner, typename std::is_same<TNew, void>::type());
}

} // namespace detail

/// @brief Then 实现：注册上游续接，成功则投递变换，失败则传播错误。
///
/// 变换函数返回普通值时同步变换；返回 CTask 时扁平化（flatMap）。
///
/// @tparam TValue 本任务的值类型。
/// @tparam TError 错误类型。
/// @tparam TFn    变换函数类型。
template <typename TValue, typename TError>
template <typename TFn>
auto CTask<TValue, TError>::Then(TFn f)
    -> CTask<typename detail::UnwrapTask<
        typename detail::TInvokeResult<TFn, TValue>::type>::type, TError>
{
    using TResult = typename detail::TInvokeResult<TFn, TValue>::type; // 变换函数原始返回类型。
    using TOut = typename detail::UnwrapTask<TResult>::type;           // 解包后下游结果类型。
    CTask<TOut, TError> taskNext;
    taskNext.m_pExecutor = m_pExecutor; // 沿用上游执行器句柄（续接投递用）。
    auto pNextState = taskNext.m_pState;
    auto pExecutor = m_pExecutor;

    m_pState->AddContinuation(
        [pExecutor, pNextState, f](const CTaskResult<TValue, TError>& upResult)
        {
            // ① 上游失败：直接转发错误对象（支持任意 TError），不执行变换。
            if (upResult.Failed())
            {
                pNextState->Complete(CTaskResult<TOut, TError>::Failure(upResult.Error()));
                return;
            }
            // ② 拷贝值，供异步续接安全使用（不引用上游共享状态）。
            TValue valueCopied = upResult.Value();
            std::function<void()> fnRun = [pNextState, f, valueCopied]()
            {
                // ③ 执行变换：普通值 → 同步完成；CTask → flatMap。
                detail::RunTransform(pNextState, f, valueCopied,
                    typename detail::IsTask<TResult>::type());
            };
            // ④ 在执行器上执行；无执行器时内联执行。
            if (pExecutor != nullptr && pExecutor->m_pPool != nullptr)
            {
                if (pExecutor->m_bStopped || !pExecutor->m_pPool->Submit(fnRun))
                {
                    pNextState->Complete(CTaskResult<TOut, TError>::Failure(
                        detail::MakeErrorFromCode<TError>(kExecutorStopped, "CAsyncExecutor 已停止")));
                }
            }
            else
            {
                fnRun();
            }
        });
    return taskNext;
}

/// @brief OnSuccess 实现：注册成功回调（fire-and-forget）。
///
/// @tparam TValue 本任务的值类型。
/// @tparam TError 错误类型。
template <typename TValue, typename TError>
void CTask<TValue, TError>::OnSuccess(const std::function<void(const TValue&)>& fnCallback)
{
    m_pState->AddContinuation(
        [fnCallback](const CTaskResult<TValue, TError>& result)
        {
            if (result.Ok() && fnCallback)
            {
                fnCallback(result.Value());
            }
        });
}

/// @brief OnFailure 实现：注册失败回调（fire-and-forget）。
///
/// @tparam TValue 本任务的值类型。
/// @tparam TError 错误类型。
template <typename TValue, typename TError>
void CTask<TValue, TError>::OnFailure(const std::function<void(const TError&)>& fnCallback)
{
    m_pState->AddContinuation(
        [fnCallback](const CTaskResult<TValue, TError>& result)
        {
            if (result.Failed() && fnCallback)
            {
                fnCallback(result.Error());
            }
        });
}

/// @brief OnSuccess 实现（CTask&lt;void, TError&gt; 特化）：注册成功回调（fire-and-forget，无参数）。
///
/// @tparam TError 错误类型。
template <typename TError>
void CTask<void, TError>::OnSuccess(const std::function<void()>& fnCallback)
{
    m_pState->AddContinuation(
        [fnCallback](const CTaskResult<void, TError>& result)
        {
            if (result.Ok() && fnCallback)
            {
                fnCallback();
            }
        });
}

/// @brief OnFailure 实现（CTask&lt;void, TError&gt; 特化）：注册失败回调（fire-and-forget）。
///
/// @tparam TError 错误类型。
template <typename TError>
void CTask<void, TError>::OnFailure(const std::function<void(const TError&)>& fnCallback)
{
    m_pState->AddContinuation(
        [fnCallback](const CTaskResult<void, TError>& result)
        {
            if (result.Failed() && fnCallback)
            {
                fnCallback(result.Error());
            }
        });
}

/// @brief Submit 实现：把任务函数投递到线程池执行。
///
/// 任务函数的返回值作为任务结果；内部异常捕获并转为 TError 错误。
///
/// @tparam TError 错误类型。
/// @tparam TFn    任务函数类型。
template <typename TError>
template <typename TFn>
auto CAsyncExecutor<TError>::Submit(TFn f)
    -> CTask<typename detail::TInvokeResult<TFn>::type, TError>
{
    using TResult = typename detail::TInvokeResult<TFn>::type; // 任务结果类型。
    CTask<TResult, TError> task;
    task.m_pExecutor = m_pHandle; // 共享执行器句柄（任务链持有时线程池不释放）。
    auto pState = task.m_pState;
    auto pHandle = m_pHandle;
    std::function<void()> fnRun = [pState, f]()
        {
            try
            {
                // 执行任务；成功完成（值或 void）。f 是调用方任意代码，可能抛异常：
                // 捕获并转为错误码，防止异常逃逸到工作线程（std::terminate）。
                detail::CompleteSuccess(pState, f);
            }
            catch (const std::exception& e)
            {
                pState->Complete(CTaskResult<TResult, TError>::Failure(
                    detail::MakeErrorFromCode<TError>(kTaskFailed, e.what())));
            }
            catch (...)
            {
                pState->Complete(CTaskResult<TResult, TError>::Failure(
                    detail::MakeErrorFromCode<TError>(kTaskFailed, "unknown error")));
            }
        };
    if (!pHandle || pHandle->m_bStopped || !pHandle->m_pPool || !pHandle->m_pPool->Submit(fnRun))
    {
        // 执行器不可用：任务立即以「未启动」错误完成。
        pState->Complete(CTaskResult<TResult, TError>::Failure(
            detail::MakeErrorFromCode<TError>(kExecutorNotStarted, "CAsyncExecutor 未启动")));
    }
    return task;
}

/// @brief 创建异步执行器（无异常版）。
///
/// @tparam TError 错误类型。
/// @param nThreadCount 工作线程数量。
template <typename TError>
CAsyncExecutor<TError>::CAsyncExecutor(size_t nThreadCount) : m_nThreadCount(nThreadCount)
{
}

/// @brief 销毁异步执行器（停止并等待任务完成）。
template <typename TError>
CAsyncExecutor<TError>::~CAsyncExecutor()
{
    Stop();
}

/// @brief 启动工作线程。
template <typename TError>
bool CAsyncExecutor<TError>::Start()
{
    if (m_pHandle)
    {
        return false;
    }
    if (m_nThreadCount == 0)
    {
        return false;
    }
    m_pHandle.reset(new detail::CExecutorHandle());
    m_pHandle->m_pPool.reset(new common::CThreadPool(m_nThreadCount));
    if (!m_pHandle->m_pPool->Start())
    {
        m_pHandle.reset();
        return false;
    }
    return true;
}

/// @brief 提交无返回值任务。
template <typename TError>
bool CAsyncExecutor<TError>::Post(const std::function<void()>& fnTask)
{
    std::shared_ptr<detail::CExecutorHandle> pHandle = m_pHandle;
    if (!pHandle || pHandle->m_bStopped || !pHandle->m_pPool)
    {
        return false;
    }
    return pHandle->m_pPool->Submit(fnTask);
}

/// @brief 停止并等待任务完成（优雅关闭）。
template <typename TError>
void CAsyncExecutor<TError>::Stop()
{
    std::shared_ptr<detail::CExecutorHandle> pHandle = m_pHandle;
    if (!pHandle)
    {
        return;
    }
    pHandle->m_bStopped = true;
    if (pHandle->m_pPool)
    {
        pHandle->m_pPool->Stop();
    }
    m_pHandle.reset();
}

/// @brief 是否正在运行。
template <typename TError>
bool CAsyncExecutor<TError>::IsRunning() const
{
    return m_pHandle != nullptr && !m_pHandle->m_bStopped &&
           m_pHandle->m_pPool != nullptr && m_pHandle->m_pPool->IsRunning();
}

} // namespace nothrow
} // namespace common
