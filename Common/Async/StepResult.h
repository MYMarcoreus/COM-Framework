#pragma once

// ====================================================================
// 层结果（CStepResult）—— 异步链层与层之间传递的唯一信息
//
// 设计要点（本分支的异步特化）：
//  - 层与层之间 **不传递任意值**，只传递「本层成功 / 失败」；
//  - 层之间需要共享的数据统一放在共享上下文（shared_ptr<TContext>，见
//    AsyncChain.h），由整条链的所有层共用同一实例；
//  - 层函数签名固定为
//        CStepResult fn(CStepResult upStep,
//                       const std::shared_ptr<TContext>& spContext);
//    下一层拿到 upStep 即可判断上一层回调是成功还是失败；
//  - 失败即停：某一层返回失败后，后续层不再执行，失败码沿链透传到收尾回调
//    与 Get()（与 Promise / C# async 的失败传播一致）。
//
// 错误码约定：框架保留 1..kStepBusinessBase 之前的区间（见 StepCode），
// 业务错误码自行从 kStepBusinessBase 起取，框架不解释业务码含义。
// ====================================================================

namespace common {
namespace async {

/// @brief 层结果码（框架保留区间）。
///
/// 业务错误码请自行从 kStepBusinessBase 起取；框架只解释本枚举内的码，
/// 其余码原样透传。
enum StepCode
{
    kStepOk = 0,        ///< 成功。
    kStepFailed = 1,    ///< 业务失败（未指定错误码时的默认值）。
    kStepStopped = 2,   ///< 执行器已停止 / 投递失败（框架）。
    kStepException = 3  ///< 层函数抛出异常（框架捕获，转为失败）。
};

/// @brief 业务错误码起始值（框架保留 0..99）。
const int kStepBusinessBase = 100;

/// @brief 层执行结果（成功 / 失败）。
///
/// 层间传递的唯一信息：成功（Ok）或失败（含错误码）。默认构造为成功，
/// 因此层函数「什么都不做就 return」即表示本层成功。
///
/// 用法：
/// @code
/// CStepResult StepLoad(CStepResult upStep, const std::shared_ptr<CMyContext>& spCtx)
/// {
///     if (upStep.IsFailed())
///     {
///         return upStep;                       // 上一层失败：原样透传
///     }
///     if (!spCtx->LoadFromDisk("data.bin"))
///     {
///         return CStepResult::Failed(kMyLoadFailed);  // 本层失败：终止链
///     }
///     return CStepResult::Ok();                // 本层成功：继续下一层
/// }
/// @endcode
class CStepResult
{
   public:
    /// @brief 默认构造：成功。
    CStepResult() : m_nCode(kStepOk) {}

    /// @brief 从错误码构造（0 即成功）。
    ///
    /// @param nCode 错误码（kStepOk 表示成功）。
    explicit CStepResult(int nCode) : m_nCode(nCode) {}

    /// @brief 成功结果。
    static CStepResult Ok() { return CStepResult(kStepOk); }

    /// @brief 失败结果。
    ///
    /// @param nCode 错误码（默认为 kStepFailed；业务码建议从 kStepBusinessBase 起取）。
    static CStepResult Failed(int nCode = kStepFailed) { return CStepResult(nCode); }

    /// @brief 是否成功。
    bool IsOk() const { return m_nCode == kStepOk; }

    /// @brief 是否失败（IsOk() 取反）。
    bool IsFailed() const { return m_nCode != kStepOk; }

    /// @brief 错误码（kStepOk 表示成功）。
    int Code() const { return m_nCode; }

    /// @brief 相等比较（按错误码）。
    ///
    /// @param other 另一结果。
    bool operator==(const CStepResult& other) const { return m_nCode == other.m_nCode; }

    /// @brief 不等比较（按错误码）。
    ///
    /// @param other 另一结果。
    bool operator!=(const CStepResult& other) const { return m_nCode != other.m_nCode; }

   private:
    int m_nCode;  ///< 错误码（kStepOk = 成功）。
};

}  // namespace async
}  // namespace common
