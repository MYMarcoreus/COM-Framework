#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>

#include "Thread/ThreadPool.h"

// ====================================================================
// 彻底无异常版异步框架（零 try/catch/throw，可在 -fno-exceptions 下编译）
//
// 与 Common/Async/AsyncExecutorNoThrow.h（对外无异常、内部 try/catch 兜底）的区别：
//  - 本文件不含任何 try/catch/throw，错误统一用「错误码 + 消息」传递；
//  - 任务函数（Submit/Then 的入参 f）必须保证不抛出异常，框架不兜底：
//    若任务函数 throw，带异常编译时异常会逃逸到工作线程导致进程终止（未定义行为）；
//  - 可在 -fno-exceptions 下编译（配合不抛异常的标准库用法）。
//
// 用法示例：
// @code
//   common::strict::CAsyncExecutor exec(2);
//   exec.Start();
//
//   // 链式调用：Submit → Then → Then，Get 返回结果（不抛异常）
//   common::strict::CTaskResult<int> r =
//       exec.Submit([]() { return 3; })
//           .Then([](int n) { return n * 2; })
//           .Then([](int n) { return n + 1; })
//           .Get();
//   if (r.Ok())     { /* 成功，取值 r.Value() */ }
//   if (r.Failed()) { /* 失败，取错误 r.Error().nCode / r.Error().strMessage */ }
// @endcode
// ====================================================================

namespace common {
namespace strict {

class CAsyncExecutor; // 前向声明（CTask 持有其指针，定义在下方）

/// @brief 任务错误码（彻底无异常的错误通道）。
///
/// 框架内置的错误类别；业务可自定义错误码（见 CTaskError::nCode 说明）。
/// 任务失败时通过错误码标识失败类别，配合 CTaskError::strMessage 携带具体原因。
/// 注意：本版不含「任务函数抛异常」的错误来源——任务函数被约定为禁止 throw，
/// 因此没有 kTaskFailed（相比 AsyncExecutorNoThrow.h 删除了该错误码）。
enum TaskErrorCode
{
    kTaskOk = 0,         // 成功（无错误）。
    kExecutorNotStarted, // 执行器未启动（Submit 时线程池不存在/未运行）。
    kExecutorStopped     // 执行器已停止（Then 投递时线程池已关闭）。
};

/// @brief 任务错误信息（彻底无异常的错误通道）。
///
/// 由「错误码 + 可读消息」组成：错误码见 TaskErrorCode 枚举，
/// 消息携带具体原因。
///
/// @note nCode 用 int 而非 TaskErrorCode：TaskErrorCode 只是框架内置的
///       建议错误码集合；用 int 允许业务方传入自定义错误码（如 1000+ 业务码），
///       便于扩展。TaskErrorCode 枚举值可隐式转 int，直接传 kExecutorNotStarted 等亦可。
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

/// @brief 任务结果（类似 std::expected<T, Error>）。
///
/// 一个结果要么「成功携带值」、要么「失败携带错误」，二者互斥：
///  - Ok()     为 true 时，可经 Value() 取成功值；
///  - Failed() 为 true 时，可经 Error() 取错误信息。
/// 通过静态工厂 Success / Failure 创建。
///
/// @tparam TValue 成功时携带的值类型。
template <typename TValue>
class CTaskResult
{
public:
    /// 创建成功结果（携带 value）。
    ///
    /// @param value 成功值（拷贝存入结果）。
    static CTaskResult<TValue> Success(const TValue& value)
    {
        CTaskResult<TValue> r;
        r.m_pValue.reset(new TValue(value));
        return r;
    }

    /// 创建失败结果（携带错误码与消息）。
    ///
    /// @param nCode      错误码（TaskErrorCode 内置值或业务自定义值）。
    /// @param strMessage 错误描述。
    static CTaskResult<TValue> Failure(int nCode, const std::string& strMessage)
    {
        CTaskResult<TValue> r;
        r.m_error = CTaskError(nCode, strMessage);
        return r;
    }

    /// 是否成功（持有值）。
    bool Ok() const { return m_pValue != nullptr; }

    /// 是否失败（未持有值）。
    bool Failed() const { return m_pValue == nullptr; }

    /// 成功时的值（仅在 Ok() 为 true 时调用）。
    const TValue& Value() const { return *m_pValue; }

    /// 失败时的错误（仅在 Failed() 为 true 时调用）。
    const CTaskError& Error() const { return m_error; }

    /// 空结果（默认 kTaskOk、无值）；通常用 Success / Failure 工厂创建。
    CTaskResult() : m_error(kTaskOk, "") {}

private:
    std::shared_ptr<TValue> m_pValue; // 成功值（非空 ⇔ 成功）。
    CTaskError m_error;               // 错误信息（失败时有效）。
};

/// @brief CTaskResult&lt;void&gt; 特化（成功无值，仅携带成败与错误）。
///
/// 用于无返回值的任务：成功时无值可取，仅 Ok() 为 true；失败时同主模板。
template <>
class CTaskResult<void>
{
public:
    /// 创建成功结果（无值）。
    static CTaskResult<void> Success()
    {
        CTaskResult<void> r;
        r.m_bOk = true;
        return r;
    }

    /// 创建失败结果（携带错误码与消息）。
    ///
    /// @param nCode      错误码（TaskErrorCode 内置值或业务自定义值）。
    /// @param strMessage 错误描述。
    static CTaskResult<void> Failure(int nCode, const std::string& strMessage)
    {
        CTaskResult<void> r;
        r.m_bOk = false;
        r.m_error = CTaskError(nCode, strMessage);
        return r;
    }

    /// 是否成功。
    bool Ok() const { return m_bOk; }

    /// 是否失败。
    bool Failed() const { return !m_bOk; }

    /// 失败时的错误（仅在 Failed() 为 true 时调用）。
    const CTaskError& Error() const { return m_error; }

    /// 空结果（默认 kTaskOk、失败态）；通常用 Success / Failure 工厂创建。
    CTaskResult() : m_bOk(false), m_error(kTaskOk, "") {}

private:
    bool m_bOk;         // 是否成功。
    CTaskError m_error; // 错误信息（失败时有效）。
};

namespace detail {

/// @brief 任务共享状态（彻底无异常版）：结果 + 续接列表 + 同步等待。
///
/// 负责管理一个任务的最终结果（CTaskResult<TValue>）、已注册的续接列表，
/// 以及阻塞等待（Wait）的同步原语。线程安全：
///  - 状态变更（Complete / AddContinuation）由 m_mutex 保护；
///  - 续接在锁外调用，避免回调内重入本状态时死锁；
///  - Wait 用 m_cv 等待就绪。
///
/// @tparam TValue 任务结果携带的值类型。
template <typename TValue>
class CTaskState
{
public:
    /// 续接回调：接收最终结果（成功值或错误）。
    using Continuation = std::function<void(const CTaskResult<TValue>&)>;

    /// 创建状态（初始未就绪）。
    CTaskState() : m_bReady(false) {}

    /// 完成并触发续接（锁外调用，防重入死锁）。
    ///
    /// 仅首次生效（重复 Complete 被忽略）；先唤醒 Wait，再按注册顺序
    /// 在锁外调用所有续接。
    ///
    /// @param result 最终结果（成功或失败）。
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
    ///
    /// 任务未完成时入队；已完成时立即在调用线程执行回调。
    ///
    /// @param fnCallback 续接回调。
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

    /// 阻塞等待结果（返回 CTaskResult，不抛异常）。
    ///
    /// 阻塞调用线程直到任务完成；返回最终结果，调用方检查 Ok()/Failed()。
    CTaskResult<TValue> Wait()
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
    CTaskResult<TValue> m_result;       // 最终结果（完成后有效）。
};

/// @brief 以「成功」完成状态（结果类型非 void，携带 f() 的返回值）。
///
/// @tparam TResult 结果值类型（非 void）。
/// @tparam TFn     任务函数类型。
template <typename TResult, typename TFn>
void CompleteSuccess(const std::shared_ptr<CTaskState<TResult> >& pState, TFn f)
{
    pState->Complete(CTaskResult<TResult>::Success(f()));
}

/// @brief 以「成功」完成状态（结果类型为 void，执行 f 但不携带值）。
///
/// @tparam TFn 任务函数类型。
template <typename TFn>
void CompleteSuccess(const std::shared_ptr<CTaskState<void> >& pState, TFn f)
{
    f();
    pState->Complete(CTaskResult<void>::Success());
}

} // namespace detail

/// @brief 异步任务（彻底无异常版），支持链式调用（Then）。
///
/// 由 CAsyncExecutor::Submit / CTask::FromResult 创建；
/// 通过 Then 串联后续步骤，Get 阻塞获取最终结果（CTaskResult，不抛异常）。
/// 错误以 CTaskResult 沿链传播：上游失败时 Then 不再执行，错误传递给下游。
///
/// @tparam TValue 任务携带的值类型。
template <typename TValue>
class CTask
{
public:
    /// 创建空任务（未完成）。
    CTask()
        : m_pExecutor(nullptr),
          m_pState(std::make_shared<detail::CTaskState<TValue> >())
    {
    }

    /// 从已就绪结果创建任务（可启动链式调用）。
    ///
    /// @param result 已就绪的结果（成功或失败）。
    static CTask<TValue> FromResult(const CTaskResult<TValue>& result)
    {
        CTask<TValue> task;
        task.m_pState->Complete(result);
        return task;
    }

    /// 链式续接：上游成功后在执行器上运行 fnTransform(value)，
    /// 上游失败则错误传播给下游（fnTransform 不执行）。
    ///
    /// @tparam TFn 变换函数类型（接收 TValue，返回新结果类型）。
    /// @param fnTransform 变换函数。
    /// @return 下游任务（携带变换后的结果类型）。
    template <typename TFn>
    CTask<typename std::result_of<TFn(TValue)>::type> Then(TFn fnTransform);

    /// 阻塞获取最终结果（不抛异常）。
    ///
    /// 调用线程阻塞至任务完成；返回 CTaskResult，需检查 Ok()/Failed()。
    CTaskResult<TValue> Get() const { return m_pState->Wait(); }

    /// 注册成功回调（fire-and-forget，任务成功时触发）。
    ///
    /// @param fnCallback 成功回调（参数为成功值）。
    void OnSuccess(const std::function<void(const TValue&)>& fnCallback);

    /// 注册失败回调（fire-and-forget，任务失败时触发）。
    ///
    /// @param fnCallback 失败回调（参数为错误信息）。
    void OnFailure(const std::function<void(const CTaskError&)>& fnCallback);

private:
    friend class CAsyncExecutor;
    template <typename UValue> friend class CTask;

    CAsyncExecutor* m_pExecutor;                    // 关联执行器（续接投递用，可为空）。
    std::shared_ptr<detail::CTaskState<TValue> > m_pState; // 任务共享状态。
};

/// @brief 异步执行器（彻底无异常版）。
///
/// 基于线程池执行任务，支持链式调用（Submit → Then → Get）。
///
/// @note 执行器必须存续到所有任务完成后才能销毁（任务续接会投递到它的线程池）。
/// @note 任务函数必须保证不抛出异常（strict 约定，框架不兜底）。
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

    /// 提交任务并返回 CTask（任务函数必须保证不抛出异常，框架不兜底）。
    ///
    /// @tparam TFn 任务函数类型（返回新任务的结果类型）。
    /// @param f 任务函数（在工作线程上执行）。
    /// @return 关联本执行器的任务；执行器未启动时任务立即以
    ///         kExecutorNotStarted 错误完成。
    template <typename TFn>
    CTask<typename std::result_of<TFn()>::type> Submit(TFn f);

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
    std::unique_ptr<common::CThreadPool> m_pPool; // 底层线程池。
    size_t m_nThreadCount;                        // 工作线程数。
};

// ================= 模板实现 ================

/// @brief Then 实现：注册上游续接，成功则投递变换，失败则传播错误。
///
/// @tparam TValue 本任务的值类型。
/// @tparam TFn    变换函数类型。
template <typename TValue>
template <typename TFn>
CTask<typename std::result_of<TFn(TValue)>::type> CTask<TValue>::Then(TFn f)
{
    using TResult = typename std::result_of<TFn(TValue)>::type; // 下游结果类型。
    CTask<TResult> taskNext;
    taskNext.m_pExecutor = m_pExecutor; // 沿用上游执行器（续接在其线程池执行）。
    std::shared_ptr<detail::CTaskState<TResult> > pNextState = taskNext.m_pState;
    CAsyncExecutor* pExecutor = m_pExecutor;

    m_pState->AddContinuation(
        [pExecutor, pNextState, f](const CTaskResult<TValue>& upResult)
        {
            // ① 上游失败：错误（错误码 + 消息）传播给下游，不执行变换。
            if (upResult.Failed())
            {
                pNextState->Complete(CTaskResult<TResult>::Failure(
                    upResult.Error().nCode, upResult.Error().strMessage));
                return;
            }

            // ② 拷贝值，供异步续接安全使用（不引用上游共享状态）。
            TValue valueCopied = upResult.Value();
            std::function<void()> fnRun = [pNextState, f, valueCopied]()
            {
                // 执行变换；成功则完成下游（值或 void）。
                // strict 约定：变换函数禁止 throw，框架不兜底。
                detail::CompleteSuccess(pNextState,
                    [valueCopied, f]() { return f(valueCopied); });
            };

            // ③ 在执行器上执行；无执行器时内联执行。
            if (pExecutor != nullptr)
            {
                if (!pExecutor->Post(fnRun))
                {
                    pNextState->Complete(CTaskResult<TResult>::Failure(
                        kExecutorStopped, "CAsyncExecutor 已停止"));
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
template <typename TValue>
void CTask<TValue>::OnSuccess(const std::function<void(const TValue&)>& fnCallback)
{
    m_pState->AddContinuation(
        [fnCallback](const CTaskResult<TValue>& result)
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
template <typename TValue>
void CTask<TValue>::OnFailure(const std::function<void(const CTaskError&)>& fnCallback)
{
    m_pState->AddContinuation(
        [fnCallback](const CTaskResult<TValue>& result)
        {
            if (result.Failed() && fnCallback)
            {
                fnCallback(result.Error());
            }
        });
}

/// @brief Submit 实现：把任务函数投递到线程池执行。
///
/// 任务函数的返回值作为任务结果。
/// strict 约定：任务函数必须保证不抛出异常，框架不兜底。
///
/// @tparam TFn 任务函数类型。
template <typename TFn>
CTask<typename std::result_of<TFn()>::type> CAsyncExecutor::Submit(TFn f)
{
    using TResult = typename std::result_of<TFn()>::type; // 任务结果类型。
    CTask<TResult> task;
    task.m_pExecutor = this;
    std::shared_ptr<detail::CTaskState<TResult> > pState = task.m_pState;
    std::function<void()> fnRun = [pState, f]()
    {
        // 执行任务；成功完成（值或 void）。strict 约定：任务函数禁止 throw。
        detail::CompleteSuccess(pState, f);
    };
    if (!m_pPool || !m_pPool->Submit(fnRun))
    {
        // 线程池不可用：任务立即以「未启动」错误完成。
        pState->Complete(CTaskResult<TResult>::Failure(
            kExecutorNotStarted, "CAsyncExecutor 未启动"));
    }
    return task;
}

} // namespace strict
} // namespace common
