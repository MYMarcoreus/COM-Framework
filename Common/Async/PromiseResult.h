#pragma once

// ====================================================================
// CPromiseResult —— promise 结果（已兑现 / 已拒绝），层与层之间传递的唯一信息
//
// 命名与语义对齐 JS 的 Promise：
//   resolve(value)   →  CPromiseResult::Resolve()        // 已兑现
//   reject(reason)   →  CPromiseResult::Reject(nCode)    // 已拒绝（reason 用错误码表达）
//   fulfilled        →  result.IsFulfilled()
//   rejected         →  result.IsRejected()
//
// 设计要点（本分支的异步特化）：
//  - 层与层之间 **不传递任意值**，只传递「已兑现 / 已拒绝」；
//  - 层之间需要共享的数据统一放在共享上下文（std::shared_ptr<TContext>，
//    见 Promise.h），由整条 promise 链的所有层共用同一实例；
//  - 处理器签名固定为
//        CPromiseResult handler(CPromiseResult upResult,
//                              const std::shared_ptr<TContext>& spContext);
//    下一层拿到 upResult 即可判断上一层是兑现还是被拒绝；
//  - then 失败即停：某一层被拒绝后，后续 then 层不再执行，拒绝码沿链透传
//    到 catch / finally / OnSettled / Await()。
//
// 错误码约定：框架保留 1..kBusinessBase 之前的区间（见 PromiseCode），
// 业务错误码自行从 kBusinessBase 起取，框架不解释业务码含义。
// ====================================================================

namespace common {
namespace async {

/// @brief promise 结果码（框架保留区间）。
///
/// 业务错误码请自行从 kBusinessBase 起取；框架只解释本枚举内的码，
/// 其余码原样透传。
enum PromiseCode
{
    kFulfilled = 0,  ///< 已兑现（成功）。
    kRejected = 1,   ///< 已拒绝（业务失败：未指定错误码时的默认值）。
    kStopped = 2,    ///< 执行器已停止 / 投递失败（框架）。
    kException = 3   ///< 处理器抛出异常（框架捕获，转为拒绝）。
};

/// @brief 业务错误码起始值（框架保留 0..99）。
const int kBusinessBase = 100;

/// @brief promise 结果：已兑现（fulfilled）或已拒绝（rejected）。
///
/// 默认构造为「已兑现」，因此处理器「什么都不做就 return」即表示本层兑现。
///
/// 用法：
/// @code
/// CPromiseResult StepLoad(CPromiseResult upResult, const std::shared_ptr<CMyContext>& spCtx)
/// {
///     if (upResult.IsRejected())
///     {
///         return upResult;                             // 上一层被拒绝：原样透传
///     }
///     if (!spCtx->LoadFromDisk("data.bin"))
///     {
///         return CPromiseResult::Reject(kMyLoadFailed); // 本层拒绝
///     }
///     return CPromiseResult::Resolve();                 // 本层兑现
/// }
/// @endcode
class CPromiseResult
{
   public:
    /// @brief 默认构造：已兑现。
    CPromiseResult() : m_nCode(kFulfilled) {}

    /// @brief 从错误码构造（kFulfilled 即兑现）。
    ///
    /// @param nCode 错误码（kFulfilled 表示兑现）。
    explicit CPromiseResult(int nCode) : m_nCode(nCode) {}

    /// @brief 已兑现（JS: resolve）。
    static CPromiseResult Resolve() { return CPromiseResult(kFulfilled); }

    /// @brief 已拒绝（JS: reject(reason)，reason 用错误码表达）。
    ///
    /// @param nCode 错误码（默认为 kRejected；业务码建议从 kBusinessBase 起取）。
    static CPromiseResult Reject(int nCode = kRejected) { return CPromiseResult(nCode); }

    /// @brief 是否已兑现。
    bool IsFulfilled() const { return m_nCode == kFulfilled; }

    /// @brief 是否已拒绝（IsFulfilled() 取反）。
    bool IsRejected() const { return m_nCode != kFulfilled; }

    /// @brief 错误码（kFulfilled 表示兑现）。
    int Code() const { return m_nCode; }

    /// @brief 相等比较（按错误码）。
    ///
    /// @param other 另一结果。
    bool operator==(const CPromiseResult& other) const { return m_nCode == other.m_nCode; }

    /// @brief 不等比较（按错误码）。
    ///
    /// @param other 另一结果。
    bool operator!=(const CPromiseResult& other) const { return m_nCode != other.m_nCode; }

   private:
    int m_nCode;  ///< 错误码（kFulfilled = 已兑现）。
};

}  // namespace async
}  // namespace common
