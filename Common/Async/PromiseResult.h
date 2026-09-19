#pragma once

#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <type_traits>

// ====================================================================
// CPromiseResult —— promise 结果（已兑现 / 已拒绝），层与层之间传递的唯一信息
//
// 命名与语义对齐 JS 的 Promise：
//   resolve()        →  CPromiseResult::Resolve()            // 已兑现
//   reject(reason)   →  CPromiseResult::Reject(异常对象)      // 已拒绝（reason = 标准异常）
//   fulfilled        →  result.IsFulfilled()
//   rejected         →  result.IsRejected()
//
// 设计要点（本分支的异步特化）：
//  - 层与层之间 **不传递任意值**，只传递「已兑现 / 已拒绝」：框架只判 `IsFulfilled()`，
//    **没有任何错误码**（`PromiseCode` / `kBusinessBase` / `EFailureKind` / `BusinessCode()` 都已删除）；
//  - **拒绝统一用标准异常表达**（`std::exception` 派生类）：结果内部只存一个
//    `std::exception_ptr`（C++11 标准的异常持有者，能正确保住异常**动态类型**，拷贝只加引用计数）。
//    要文字用 `What()` / `Message()`，要按类型分流就 `Exception()` 重新抛出后 catch 具体类型；
//  - **详细的业务错误信息由业务上下文提供**（`TContext` 里自己放字段，想放多少放多少）：
//    结果里不带业务码、不带业务细节 —— 层间透传成本与业务错误的种类数无关；
//  - 框架自己的失败（执行器不可用 / `AwaitFor` 超时 / 处理器抛异常）同样是标准异常，
//    只是文案固定、用 `detail::FailureXxx()` 预建（见下），不另定义异常类型；
//  - 层之间需要共享的数据统一放在共享上下文（`std::shared_ptr<TContext>`，见 Promise.h）；
//  - 处理器签名 **没变**（拒绝原因藏在结果里，不占额外形参）：
//        CPromiseResult handler(CPromiseResult upResult, const std::shared_ptr<TContext>& spContext);
//  - then 失败即停：某一层被拒绝后，后续 then 层不再执行。
//
// 框架侧失败一览（写侧 = `Reject(detail::FailureXxx())`；读侧 = `Message()` 文本）：
//
//   预建失败                含义                                        文案
//   ----------------------  ------------------------------------------  --------------
//   FailureStopped()        执行器不可用 / 投递失败 / 跨执行器续接失败   「执行器已停」
//   FailureTimeout()        `AwaitFor(ms)` 超时（只报「没等到」）        「等待超时」
//   FailureHandler()        框架收口时处理器抛了非 std 异常              「处理器异常」
//   FailureUnspecified()    框架拒绝但无更具体原因（组合器空集合、         「未指定原因」
//                           起链回调缺失）
//   FailureUnknown()        拿到空的 `std::exception_ptr`（不该发生）     「未知异常」
//
// 开销（实测）：
//  - `sizeof` = 8 字节（一个 `std::exception_ptr`）；
//  - 按值在层间透传 = 引用计数 +1，**零分配**；
//  - 框架侧拒绝（预建的 exception_ptr）= 构造 + 透传都**零分配**；
//  - 业务拒绝 = 建异常那一处分配（异常对象 + 文案缓冲），之后透传只加引用计数
//    （护栏见 Tests/test_async_alloc.cpp）。
// ====================================================================

namespace common {
namespace async {

namespace detail {

/// @brief 预建框架侧失败：「执行器已停」（执行器不可用 / 投递失败 / 跨执行器续接失败）。
///
/// 进程级共享（`static` 局部量的 magic static，首次使用时线程安全地构造一次）→
/// 之后每次拒绝只是拷贝一个 `std::exception_ptr`（引用计数 +1），**零分配**。
///
/// @return 共享的异常持有者。
inline const std::exception_ptr& FailureStopped()
{
    static const std::exception_ptr s_pException = std::make_exception_ptr(std::runtime_error("执行器已停"));
    return s_pException;
}

/// @brief 预建框架侧失败：「等待超时」（`AwaitFor(ms)` 没等到）。
///
/// @return 共享的异常持有者。
inline const std::exception_ptr& FailureTimeout()
{
    static const std::exception_ptr s_pException = std::make_exception_ptr(std::runtime_error("等待超时"));
    return s_pException;
}

/// @brief 预建框架侧失败：「处理器异常」（框架收口时处理器抛了非 std 异常）。
///
/// @return 共享的异常持有者。
inline const std::exception_ptr& FailureHandler()
{
    static const std::exception_ptr s_pException = std::make_exception_ptr(std::runtime_error("处理器异常"));
    return s_pException;
}

/// @brief 预建框架侧失败：「未指定原因」（组合器空集合的 race / any、起链回调缺失）。
///
/// @return 共享的异常持有者。
inline const std::exception_ptr& FailureUnspecified()
{
    static const std::exception_ptr s_pException = std::make_exception_ptr(std::runtime_error("未指定原因"));
    return s_pException;
}

/// @brief 预建框架侧失败：「未知异常」（拿到空的 `std::exception_ptr`，不该发生）。
///
/// @return 共享的异常持有者。
inline const std::exception_ptr& FailureUnknown()
{
    static const std::exception_ptr s_pException = std::make_exception_ptr(std::runtime_error("未知异常"));
    return s_pException;
}

}  // namespace detail

/// @brief promise 结果：已兑现（fulfilled）或已拒绝（rejected，携带标准异常）。
///
/// 默认构造为「已兑现」，因此处理器「什么都不做就 return」即表示本层兑现。
///
/// 用法：
/// @code
/// CPromiseResult StepLoad(CPromiseResult upResult, const std::shared_ptr<CMyContext>& spCtx)
/// {
///     (void)upResult;  // then 层不看上游结果：上一层被拒绝时框架直接跳过本层（失败即停）
///     if (!spCtx->LoadFromDisk("data.bin"))
///     {
///         // 业务拒绝：标准异常表达原因；细节（路径 / errno / 重试次数……）写在业务上下文里
///         spCtx->strError = "读盘失败: " + spCtx->strPath;
///         return CPromiseResult::Reject(std::runtime_error(spCtx->strError));
///     }
///     return CPromiseResult::Resolve();  // 本层兑现
/// }
/// @endcode
class CPromiseResult
{
public:
    /// @brief 默认构造：已兑现。
    CPromiseResult() : m_pException()
    {}

    /// @brief 已兑现（JS: resolve）。
    ///
    /// @return 已兑现的结果。
    static CPromiseResult Resolve()
    {
        return CPromiseResult();
    }

    //================ 拒绝：**统一就叫 Reject** ================

    /// @brief 已拒绝：携带异常对象（JS: reject(reason)）。
    ///
    /// 异常对象被拷进 `std::exception_ptr`（`make_exception_ptr`，**静态类型即异常类型**，
    /// 所以 `Reject(std::runtime_error("..."))` 的文案不会因为切片而丢 —— 拿到的 `what()` 就是它）。
    ///
    /// 注意：在 `catch (const std::exception& e)` 里**不要**写 `Reject(e)` —— 形参的静态类型已经是
    /// `std::exception`，动态类型会被切掉；那种场合用 `Reject(std::current_exception())`。
    ///
    /// @tparam TException 异常类型（必须是 `std::exception` 的派生类）。
    /// @param exception 异常对象（按 `TException` 拷一份）。
    /// @return 已拒绝的结果。
    template <typename TException>
    static typename std::enable_if<std::is_base_of<std::exception, TException>::value, CPromiseResult>::type Reject(
        const TException& exception)
    {
        return CPromiseResult(std::make_exception_ptr(exception));
    }

    /// @brief 已拒绝：携带现成的 `std::exception_ptr`（`catch` 里透传异常用这个）。
    ///
    /// 传空 exception_ptr 时按「未指定原因」处理：**不会**被当成兑现。
    ///
    /// @param pException 异常持有者（通常是 `std::current_exception()`）。
    /// @return 已拒绝的结果。
    static CPromiseResult Reject(std::exception_ptr pException)
    {
        return CPromiseResult(pException != nullptr ? pException : detail::FailureUnknown());
    }

    //================ 读侧 ================

    /// @brief 是否已兑现（**框架内部只判这一个**）。
    ///
    /// @return true 已兑现。
    bool IsFulfilled() const
    {
        return m_pException == nullptr;
    }

    /// @brief 是否已拒绝。
    ///
    /// @return true 已拒绝。
    bool IsRejected() const
    {
        return m_pException != nullptr;
    }

    /// @brief 异常持有者（兑现时为空）。
    ///
    /// 想按类型分流时用它重新抛出后 catch 具体类型：
    /// @code
    /// try
    /// {
    ///     std::rethrow_exception(result.Exception());
    /// }
    /// catch (const std::runtime_error& e)
    /// {
    ///     // 只处理这一类
    /// }
    /// @endcode
    ///
    /// @return 异常持有者（可空）。
    std::exception_ptr Exception() const
    {
        return m_pException;
    }

    /// @brief 异常描述（`what()`）。
    ///
    /// 返回的指针指向异常对象内部的字符串，**在本结果存活期间有效**（异常对象由
    /// `std::exception_ptr` 持有）；兑现时返回空串。需要独立字符串请用 `Message()`。
    ///
    /// @return 描述（兑现 = 空串，不会返回 nullptr）。
    const char* What() const
    {
        if (m_pException == nullptr)
        {
            return "";
        }
        try
        {
            std::rethrow_exception(m_pException);
        }
        catch (const std::exception& e)
        {
            return e.what();
        }
        catch (...)
        {
            return "未知异常";  // 非 std::exception 的异常（框架内部不会造）
        }
    }

    /// @brief 异常描述的副本（日志 / 断言用；每次调用拷一份字符串）。
    ///
    /// @return 描述（兑现 = 空串）。
    std::string Message() const
    {
        return std::string(What());
    }

    /// @brief 相等比较：**按「兑现 / 拒绝」+ 异常描述**。
    ///
    /// @param other 另一结果。
    /// @return true 相等。
    bool operator==(const CPromiseResult& other) const
    {
        if (IsRejected() != other.IsRejected())
        {
            return false;
        }
        return IsFulfilled() || std::strcmp(What(), other.What()) == 0;
    }

    /// @brief 不等比较。
    ///
    /// @param other 另一结果。
    /// @return true 不等。
    bool operator!=(const CPromiseResult& other) const
    {
        return !(*this == other);
    }

private:
    /// @brief 内部构造（唯一的成员就是异常持有者：空 = 已兑现）。
    ///
    /// @param pException 异常持有者（空 = 已兑现）。
    explicit CPromiseResult(std::exception_ptr pException) : m_pException(pException)
    {}

    std::exception_ptr m_pException;  ///< 拒绝原因（空 = 已兑现）。
};

}  // namespace async
}  // namespace common
