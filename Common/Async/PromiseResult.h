#pragma once

#include <cstring>
#include <exception>
#include <memory>
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
//  - **拒绝统一用标准异常表达**（`std::exception` 派生类）；结果内部只存一个
//    `std::shared_ptr<const std::exception>`：
//      * **共享一个不可变的异常对象**（引用计数），拷贝便宜、跨线程安全；
//      * **保住动态类型**（要按类型分流就 `dynamic_cast<const CMyError*>(result.Exception().get())`）；
//      * 读文本就是一次**虚函数调用**（`what()`）—— 不需要 `rethrow`、不走 unwinder、零分配
//        （对比 `std::exception_ptr` 的 `rethrow_exception`：约 **500 ns** vs **~1 ns**）；
//  - **详细的业务错误信息由业务上下文提供**（`TContext` 里自己放字段，想放多少放多少）：
//    结果里不带业务码、不带业务细节 —— 层间透传成本与业务错误的种类数无关；
//  - 框架自己的失败（执行器不可用 / `AwaitFor` 超时 / 处理器抛异常）同样是标准异常，
//    文案固定（「执行器已停」「等待超时」「处理器异常」「未指定原因」），**在调用点直接构造**；
//  - 层之间需要共享的数据统一放在共享上下文（`std::shared_ptr<TContext>`，见 Promise.h）；
//  - 处理器签名 **没变**（拒绝原因藏在结果里，不占额外形参）：
//        CPromiseResult handler(CPromiseResult upResult, const std::shared_ptr<TContext>& spContext);
//  - then 失败即停：某一层被拒绝后，后续 then 层不再执行。
//
// 唯一的边界：**装不下「正在飞的异常」**
//  `std::exception_ptr` 能用 `std::current_exception()` 零拷贝接手一个刚抛出的异常，
//  `shared_ptr` 不行（它必须自己拥有对象）。所以在 `catch` 里收口时要**重新构造**一份：
//      catch (const std::exception& e) { result = CPromiseResult::Reject(std::runtime_error(e.what())); }
//  文本（`what()`）完整保留，但**动态类型降级为 `std::runtime_error`** ——
//  要保证类型可分流，请在层里 `return CPromiseResult::Reject(CMyError(...))` **构造**拒绝。
//
// 开销（实测，`-O2`）：
//  - `sizeof` = 16 字节（一个 `std::shared_ptr`）；
//  - 按值在层间**透传** = 引用计数 +1，**零分配**（不管拒绝怎么造出来的）；
//  - 造一个拒绝 = `make_shared`（对象 + 控制块合并）**1 次分配**；短文案走 SSO 不再额外分配；
//  - 读文本（`What()` / `Message()`）~1 ns / ~12 ns，**零分配**。
// ====================================================================

namespace common {
namespace async {

/// @brief promise 结果：已兑现（fulfilled）或已拒绝（rejected，携带一个共享的标准异常对象）。
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
    CPromiseResult() : m_spException()
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
    /// 异常对象被**拷进一份共享的不可变副本**（`make_shared<const TException>`）：
    /// **静态类型即异常类型**，所以 `Reject(std::runtime_error("..."))` 不会因为切片而丢文案，
    /// 之后 `dynamic_cast<const TException*>(result.Exception().get())` 也能按类型分流。
    ///
    /// @tparam TException 异常类型（必须是 `std::exception` 的派生类）。
    /// @param exception 异常对象（按 `TException` 拷一份）。
    /// @return 已拒绝的结果。
    template <typename TException>
    static typename std::enable_if<std::is_base_of<std::exception, TException>::value, CPromiseResult>::type Reject(
        const TException& exception)
    {
        // 只做一件事：把异常按**静态类型** TException 拷进一份共享的不可变副本。
        // 为什么不直接存引用 / 指针：结果要在层间按值传递、可跨线程，所以必须自持且只读。
        return CPromiseResult(std::make_shared<const TException>(exception));
    }


    //================ 读侧 ================

    /// @brief 是否已兑现（**框架内部只判这一个**）。
    ///
    /// @return true 已兑现。
    bool IsFulfilled() const
    {
        return m_spException == nullptr;
    }

    /// @brief 是否已拒绝。
    ///
    /// @return true 已拒绝。
    bool IsRejected() const
    {
        return m_spException != nullptr;
    }

    /// @brief 异常对象（兑现时为空）。
    ///
    /// 想按类型分流时直接 `dynamic_cast`（不需要 `catch`、不走 unwinder）：
    /// @code
    /// if (const CDbError* pErr = dynamic_cast<const CDbError*>(result.Exception().get()))
    /// {
    ///     // 只处理这一类（pErr->Kind() / pErr->what()）
    /// }
    /// @endcode
    ///
    /// @return 异常对象的共享句柄（可空；指向的对象**不可变**，可跨线程共享）。
    const std::shared_ptr<const std::exception>& Exception() const
    {
        return m_spException;
    }

    /// @brief 异常描述（`what()`）。
    ///
    /// 就是一次虚函数调用：返回的指针指向异常对象内部的字符串，**在本结果存活期间有效**
    /// （异常对象由 `shared_ptr` 保活）；兑现时返回空串。需要独立字符串请用 `Message()`。
    ///
    /// @return 描述（兑现 = 空串，不会返回 nullptr）。
    const char* What() const
    {
        return m_spException != nullptr ? m_spException->what() : "";
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
        // ① 先比成败：一兑现一失败 → 直接不等（连带把「两者都兑现」筛出来，不必比文案）。
        if (IsRejected() != other.IsRejected())
        {
            return false;
        }

        // ② 再比异常描述（都兑现时 `What()` 都是空串，无需特判）。
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
    /// @brief 内部构造（唯一的成员就是异常对象：空 = 已兑现）。
    ///
    /// @param spException 异常对象的共享句柄（空 = 已兑现）。
    explicit CPromiseResult(std::shared_ptr<const std::exception> spException) : m_spException(spException)
    {}

    std::shared_ptr<const std::exception> m_spException;  ///< 拒绝原因（空 = 已兑现）。
};

}  // namespace async
}  // namespace common
