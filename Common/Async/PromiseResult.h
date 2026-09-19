#pragma once

#include <memory>
#include <string>

#include "Assert.h"

// ====================================================================
// CPromiseResult —— promise 结果（已兑现 / 已拒绝），层与层之间传递的唯一信息
//
// 命名与语义对齐 JS 的 Promise：
//   resolve(value)   →  CPromiseResult::Resolve()               // 已兑现
//   reject(reason)   →  CPromiseResult::Reject(CRefusal(...))   // 已拒绝（reason = 码 + 文案）
//   fulfilled        →  result.IsFulfilled()
//   rejected         →  result.IsRejected()
//
// 设计要点（本分支的异步特化）：
//  - 层与层之间 **不传递任意值**，只传递「已兑现 / 已拒绝」；
//  - 拒绝的载荷是 CRefusal（**码 + 文案 + 来源**），沿链原样透传到 catch / finally /
//    OnSettled / Await() —— 与异常一样「自带说明」，不必再维护一张码表才能打日志；
//  - 层之间需要共享的数据统一放在共享上下文（std::shared_ptr<TContext>，
//    见 Promise.h），由整条 promise 链的所有层共用同一实例；
//  - 处理器签名固定为
//        CPromiseResult handler(CPromiseResult upResult,
//                              const std::shared_ptr<TContext>& spContext);
//    下一层拿到 upResult 即可判断上一层是兑现还是被拒绝；
//  - then 失败即停：某一层被拒绝后，后续 then 层不再执行。
//
// 错误码：**框架不解释业务码**。业务码可以取任意 int（含 0/1/2/3），因为「是否兑现」由结果
// 自身表示（IsFulfilled()），**不是**靠「码 == 0」判定 —— 所以不存在「业务码必须从某个数
// 开始」这类约定，也不存在「框架保留区间」。
//
// 拒绝来源（ERefusalFrom）由**谁构造这个拒绝**决定，不看数值区间：
//
//   来源        码          含义与产生点（框架侧按这张表收口）
//   ----------  ----------  --------------------------------------------------------------------
//   kFramework  kRejected   未指定原因：组合器空集合的 race / any；
//                           NewPromise(spCtx, fnStarter) 没给起链回调
//   kFramework  kStopped    ① 执行器不可用 / 投递失败（本层收口）；
//                           ② AwaitFor(ms) 超时（只报「没等到」，不落定本层、链仍在后台跑）
//   kFramework  kException  处理器 / 起链回调 / 子链工厂 / 搬运抛异常（带 what 文案）
//   kBusiness   任意 int    业务模块自己决定；框架只透传
//
// 分流建议：`IsFromFramework()` 是系统侧失败（通常记日志后以业务语义向调用方收口），
// 否则是业务拒绝。`Code()` 照旧可用，但**不要用 `Code() == 0` 判兑现**（业务码可以是 0）。
// ====================================================================

namespace common {
namespace async {

/// @brief 框架侧拒绝码（仅在 `CRefusal::From() == ERefusalFrom::kFramework` 时出现）。
enum PromiseCode
{
    kFulfilled = 0,  ///< 已兑现 —— `Code()` 在兑现时的返回值（**不是**拒绝码）。
    kRejected = 1,   ///< 已拒绝：未指定原因。
    kStopped = 2,    ///< 执行器已停止 / 投递失败 / 等待超时。
    kException = 3   ///< 处理器（或起链回调 / 子链工厂 / 搬运）抛出异常。
};

/// @brief 拒绝来源：由「谁构造了这个拒绝」决定，不靠数值区间判定。
enum class ERefusalFrom
{
    kBusiness = 0,  ///< 业务模块产生（框架不解释其码与文案）。
    kFramework      ///< 框架产生（码取 PromiseCode）。
};

/// @brief 拒绝原因（`reject(reason)` 的 reason）：错误码 + 错误文案 + 来源。
///
/// 业务侧直接构造：
/// @code
/// return CPromiseResult::Reject(CRefusal(kCodeNoAccount, "账户不存在"));
/// @endcode
///
/// 框架侧用三个工厂（`Rejected()` / `Stopped()` / `Exception(what)`）。
class CRefusal
{
public:
    /// @brief 构造一个拒绝原因。
    ///
    /// @param nCode 错误码（业务码任意取值；框架码见 PromiseCode）。
    /// @param strMessage 人可读说明（可空；日志 / 上报用）。
    /// @param eFrom 来源（业务侧用默认值 kBusiness）。
    ///
    /// @note 刻意声明为 explicit：`Reject(5)` 这种「顺手把 int 当拒绝原因」的写法必须显式
    ///       写成 `Reject(CRefusal(5, "…"))`，避免业务码与框架码被静默混用。
    explicit CRefusal(int nCode, const std::string& strMessage = std::string(), ERefusalFrom eFrom = ERefusalFrom::kBusiness)
        : m_nCode(nCode), m_strMessage(strMessage), m_eFrom(eFrom)
    {}

    /// @brief 错误码。
    int Code() const
    {
        return m_nCode;
    }

    /// @brief 人可读说明（可空）。
    const std::string& Message() const
    {
        return m_strMessage;
    }

    /// @brief 拒绝来源。
    ERefusalFrom From() const
    {
        return m_eFrom;
    }

    /// @brief 是否框架产生的拒绝。
    bool IsFromFramework() const
    {
        return m_eFrom == ERefusalFrom::kFramework;
    }

    /// @brief 未指定原因的拒绝（框架）。
    static CRefusal Rejected()
    {
        return CRefusal(kRejected, "未指定原因", ERefusalFrom::kFramework);
    }

    /// @brief 执行器不可用 / 投递失败 / 等待超时（框架）。
    static CRefusal Stopped()
    {
        return CRefusal(kStopped, "执行器已停", ERefusalFrom::kFramework);
    }

    /// @brief 抛出异常导致的拒绝（框架）。
    ///
    /// @param pszWhat 异常文本（`std::exception::what()` 的返回值，可为 nullptr）。
    static CRefusal Exception(const char* pszWhat)
    {
        const bool bHasWhat = (pszWhat != nullptr && pszWhat[0] != '\0');
        return CRefusal(kException, bHasWhat ? pszWhat : "处理器异常", ERefusalFrom::kFramework);
    }

private:
    int m_nCode;               ///< 错误码。
    std::string m_strMessage;  ///< 人可读说明（可空）。
    ERefusalFrom m_eFrom;      ///< 来源。
};

/// @brief promise 结果：已兑现（fulfilled）或已拒绝（rejected，带拒绝原因）。
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
///         return CPromiseResult::Reject(CRefusal(kMyLoadFailed, "读盘失败"));  // 本层拒绝
///     }
///     return CPromiseResult::Resolve();                                       // 本层兑现
/// }
/// @endcode
class CPromiseResult
{
public:
    /// @brief 默认构造：已兑现。
    CPromiseResult() : m_bFulfilled(true), m_spRefusal()
    {}

    /// @brief 已兑现（JS: resolve）。
    static CPromiseResult Resolve()
    {
        return CPromiseResult();
    }

    /// @brief 已拒绝（JS: reject(reason)）—— 拒绝的唯一入口。
    ///
    /// @param refusal 拒绝原因（码 + 文案 + 来源）。
    static CPromiseResult Reject(const CRefusal& refusal)
    {
        CPromiseResult result;
        result.m_bFulfilled = false;
        result.m_spRefusal = std::make_shared<const CRefusal>(refusal);
        return result;
    }

    /// @brief 是否已兑现。
    bool IsFulfilled() const
    {
        return m_bFulfilled;
    }

    /// @brief 是否已拒绝。
    bool IsRejected() const
    {
        return !m_bFulfilled;
    }

    /// @brief 错误码：兑现时返回 `kFulfilled`(0)，否则是拒绝原因里的码。
    ///
    /// @note 判断兑现请用 `IsFulfilled()`：业务码可以取 0，因此**不能**用 `Code() == 0` 判兑现。
    int Code() const
    {
        return m_bFulfilled ? kFulfilled : m_spRefusal->Code();
    }

    /// @brief 拒绝文案（兑现时为空串）。
    const std::string& Message() const
    {
        return m_bFulfilled ? EmptyMessage() : m_spRefusal->Message();
    }

    /// @brief 是否框架产生的拒绝（兑现时为 false）。
    bool IsFromFramework() const
    {
        return !m_bFulfilled && m_spRefusal->IsFromFramework();
    }

    /// @brief 拒绝原因（兑现时为 nullptr）。
    const CRefusal* Refusal() const
    {
        return m_spRefusal.get();
    }

    /// @brief 拒绝原因（共享所有权版；兑现时为空）—— 需要把同一份原因交给别人保管时用。
    const std::shared_ptr<const CRefusal>& RefusalPtr() const
    {
        return m_spRefusal;
    }

    /// @brief 拒绝原因（引用版）：调用点已判 `IsRejected()` 时用，省去空判。
    const CRefusal& AsRefusal() const
    {
        ASSERT(m_spRefusal != nullptr);  // 兑现态调它是用法错误。
        return *m_spRefusal;
    }

    /// @brief 相等比较（先比「兑现 / 拒绝」，拒绝再比错误码）。
    ///
    /// @param other 另一结果。
    bool operator==(const CPromiseResult& other) const
    {
        if (m_bFulfilled != other.m_bFulfilled)
        {
            return false;
        }
        return m_bFulfilled || m_spRefusal->Code() == other.m_spRefusal->Code();
    }

    /// @brief 不等比较。
    ///
    /// @param other 另一结果。
    bool operator!=(const CPromiseResult& other) const
    {
        return !(*this == other);
    }

private:
    /// @brief 兑现时的空文案（让 `Message()` 能返回引用）。
    static const std::string& EmptyMessage()
    {
        static const std::string s_strEmpty;
        return s_strEmpty;
    }

    bool m_bFulfilled;                            ///< 是否已兑现（判兑现的唯一依据）。
    std::shared_ptr<const CRefusal> m_spRefusal;  ///< 拒绝原因（兑现时为空）。
};

}  // namespace async
}  // namespace common
