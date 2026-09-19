#pragma once

#include <memory>
#include <string>

// ====================================================================
// CPromiseResult —— promise 结果（已兑现 / 已拒绝），层与层之间传递的唯一信息
//
// 命名与语义对齐 JS 的 Promise：
//   resolve(value)   →  CPromiseResult::Resolve()                // 已兑现
//   reject(reason)   →  CPromiseResult::Reject(码, 文案)         // 已拒绝（reason = 码 + 文案）
//   fulfilled        →  result.IsFulfilled()
//   rejected         →  result.IsRejected()
//
// 设计要点（本分支的异步特化）：
//  - 层与层之间 **不传递任意值**，只传递「已兑现 / 已拒绝」；
//  - **兑现与错误码分开**：是否兑现由结果自身的标志位决定（`IsFulfilled()`），不再靠「码 == 0」——
//    所以业务码可以取 0（以及 1 / 2 / 3 以外的任意 int）；`Code()` 在兑现时返回 `kFulfilled`(0)
//  - **拒绝可以带文案**（动态字符串，任意长度）：随结果沿链透传到 catch / finally /
//    OnSettled / Await()，不必再维护一张码表才能打日志；
//  - 层之间需要共享的数据统一放在共享上下文（`std::shared_ptr<TContext>`，见 Promise.h）；
//  - 处理器签名 **没变**（文案藏在结果里，不占额外形参）：
//        CPromiseResult handler(CPromiseResult upResult, const std::shared_ptr<TContext>& spContext);
//  - then 失败即停：某一层被拒绝后，后续 then 层不再执行。
//
// 错误码：框架只解释自己产生的三个码，业务码原样透传（框架不解释其含义）：
//
//   码          含义                          产生点                         Reject(码) 自带文案
//   ----------  ----------------------------  -----------------------------  ------------------
//   kRejected   已拒绝（未指定原因）          组合器空集合的 race / any；     「未指定原因」
//                                             起链回调缺失
//   kStopped    执行器不可用 / 等超时 / 投递失败  ① PostToHandle 返回 false      「执行器已停」/「等待超时」
//               （含 kStopped 的其它框架收口）   ② AwaitFor(ms) 超时
//   kException  处理器 / 起链回调 / 子链工厂 / 搬运抛异常（文案 = 异常的 what()）  「处理器异常」
//
// **`Reject(码)` 自带框架文案**：上面三个码（外加 `AwaitFor` 超时的「等待超时」）会由
// `Reject(int)` 自动补上进程级预建的固定文案 —— 所以「只拿得到一个 int 码」的通道
// （`RejectFn` 起链回调、协程终止码、组合器首个拒绝码）也带得上文案，调用方**不必**手写码表。
//
// 分流建议：`码 < kBusinessBase` 视为系统侧失败（框架码），`>= kBusinessBase` 视为业务拒绝。
// **文案不是给机器判定的**：机器判定一律用码，文案仅供日志 / 人读。
//
// 开销（实测）：
//  - `sizeof` = 24 字节（一个 shared_ptr + int + bool）；结果按值在层间透传，透传只加引用计数；
//  - **不带文案的拒绝**（含框架侧全部拒绝 —— 文案是进程级预建的共享串）= **零分配**；
//  - 带文案的拒绝 = 两次分配（文案对象 + 字符串缓冲；短文案走 SSO 只剩一次），
//    只发生在拒绝路径；之后沿链透传只加引用计数（护栏见 Tests/test_async_alloc.cpp）。
// ====================================================================

namespace common {
namespace async {

/// @brief promise 结果码（**框架自己产生的码**；业务码由业务自定，框架不解释）。
enum PromiseCode
{
    kFulfilled = 0,  ///< 已兑现 —— `Code()` 在兑现时的返回值（**不是**拒绝码）。
    kRejected = 1,   ///< 已拒绝：未指定原因（框架）。
    kStopped = 2,    ///< 执行器已停止 / 投递失败 / 等待超时（框架）。
    kException = 3   ///< 处理器（或起链回调 / 子链工厂 / 搬运）抛出异常（框架）。
};

/// @brief 业务错误码的**建议**起点（只是编号习惯：框架不校验、也不占用区间）。
///
/// 但 **1 / 2 / 3 三个码被框架占用**：它们经 `Reject(int)` 会自动带上框架固定文案
/// （见下表），所以业务码请从 `kBusinessBase` 起（或至少避开 1 / 2 / 3）；
/// `kFulfilled`(0) 不在其中，可以当业务码用。
const int kBusinessBase = 100;

/// @brief promise 结果：已兑现（fulfilled）或已拒绝（rejected，可带错误码与文案）。
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
///         // 拒绝：码自定 + 动态文案（任意长度，随结果沿链透传）
///         return CPromiseResult::Reject(
///             kMyLoadFailed, "读盘失败: " + spCtx->strPath + "（错误 " + std::to_string(nErrCode) + "）");
///     }
///     return CPromiseResult::Resolve();  // 本层兑现
/// }
/// @endcode
class CPromiseResult
{
public:
    /// @brief 默认构造：已兑现。
    CPromiseResult() : m_spMessage(), m_nCode(kFulfilled), m_bFulfilled(true)
    {}

    /// @brief 已兑现（JS: resolve）。
    static CPromiseResult Resolve()
    {
        return CPromiseResult();
    }

    /// @brief 已拒绝（**只要错误码**；零分配）。
    ///
    /// 框架码（`kRejected` / `kStopped` / `kException`）自动补上框架固定文案 —— 文案是
    /// 进程级预建的共享串，**零分配**；其余码（业务码）原样透传、不带文案。
    /// 这也是「只拿得到一个 int 码」的通道（`RejectFn`、协程终止码、组合器首个拒绝码）的入口。
    ///
    /// @param nCode 错误码（业务码避开 1 / 2 / 3；`kFulfilled`(0) 不在其中，可当业务码用）。
    static CPromiseResult Reject(int nCode)
    {
        switch (nCode)
        {
            case kRejected:
                return CPromiseResult(false, nCode, TextRejected());
            case kStopped:
                return CPromiseResult(false, nCode, TextStopped());
            case kException:
                return CPromiseResult(false, nCode, TextException());
            default:
                return CPromiseResult(false, nCode, nullptr);  // 业务码：原样透传，不带文案。
        }
    }

    /// @brief 已拒绝（**错误码 + 文案**）。
    ///
    /// 文案是动态字符串（任意长度），随结果沿链透传：`Reject` 时拷贝一份
    /// （两次分配：文案对象 + 字符串缓冲），之后层间透传只加引用计数。
    ///
    /// @param nCode 错误码（业务码任意取值）。
    /// @param strMessage 错误文案（人读；空串 = 等同不带文案）。
    static CPromiseResult Reject(int nCode, const std::string& strMessage)
    {
        return CPromiseResult(false, nCode, strMessage.empty() ? nullptr : std::make_shared<const std::string>(strMessage));
    }

    /// @brief 已拒绝（码 + 文案；**字面量 / C 字符串版**）。
    ///
    /// 与上面同义，但直接由 `const char*` 构造共享文案，**省掉一个临时 string**。
    /// 字面量调用会优先选它（`Reject(码, "库存不足")`）。
    ///
    /// @param nCode 错误码。
    /// @param pszMessage 错误文案（可为 nullptr / 空串 = 不带文案）。
    static CPromiseResult Reject(int nCode, const char* pszMessage)
    {
        return CPromiseResult(false, nCode,
            (pszMessage == nullptr || pszMessage[0] == '\0') ? nullptr : std::make_shared<const std::string>(pszMessage));
    }

    /// @brief 是否已兑现（**判兑现的唯一依据** —— 业务码可以是 0，别看码值）。
    bool IsFulfilled() const
    {
        return m_bFulfilled;
    }

    /// @brief 是否已拒绝。
    bool IsRejected() const
    {
        return !m_bFulfilled;
    }

    /// @brief 错误码（兑现时返回 `kFulfilled`；拒绝时是拒绝时给的码）。
    int Code() const
    {
        return m_nCode;
    }

    /// @brief 错误文案（没带文案时返回空串）。
    ///
    /// @return 文案的引用（与结果共享同一份字符串，零拷贝）。
    const std::string& Message() const
    {
        static const std::string s_strEmpty;
        return m_spMessage != nullptr ? *m_spMessage : s_strEmpty;
    }

    /// @brief 相等比较：**按「兑现 / 拒绝」+ 错误码**（不比较文案）。
    ///
    /// @param other 另一结果。
    bool operator==(const CPromiseResult& other) const
    {
        return m_bFulfilled == other.m_bFulfilled && (m_bFulfilled || m_nCode == other.m_nCode);
    }

    /// @brief 不等比较。
    ///
    /// @param other 另一结果。
    bool operator!=(const CPromiseResult& other) const
    {
        return !(*this == other);
    }

private:
    /// @brief 内部构造（用预建共享文案时走它，避开重复分配）。
    ///
    /// @param bFulfilled 是否已兑现。
    /// @param nCode 错误码。
    /// @param spMessage 文案（空 = 不带文案）。
    CPromiseResult(bool bFulfilled, int nCode, const std::shared_ptr<const std::string>& spMessage)
        : m_spMessage(spMessage), m_nCode(nCode), m_bFulfilled(bFulfilled)
    {}

    /// @brief 框架固定文案「未指定原因」（进程级预建一次 → 之后零分配）。
    /// @return 共享的不可变文案。
    static const std::shared_ptr<const std::string>& TextRejected();

    /// @brief 框架固定文案「执行器已停」（进程级预建一次 → 之后零分配）。
    /// @return 共享的不可变文案。
    static const std::shared_ptr<const std::string>& TextStopped();

    /// @brief 框架固定文案「处理器异常」（进程级预建一次 → 之后零分配）。
    /// @return 共享的不可变文案。
    static const std::shared_ptr<const std::string>& TextException();

    /// 成员顺序刻意排成 8 + 4 + 1 → sizeof = 24（换顺序会变 32）。
    std::shared_ptr<const std::string> m_spMessage;  ///< 错误文案（无文案 = 空）。
    int m_nCode;                                     ///< 错误码。
    bool m_bFulfilled;                               ///< 是否已兑现（判兑现的唯一依据）。
};

// 框架固定文案：`static` 局部量（C++11 magic static，首次使用时线程安全地构造一次）→ 之后每次拒绝
// 只是拷贝一个 shared_ptr（引用计数 +1），**不分配**。取址稳定，可安全跨线程共享。

inline const std::shared_ptr<const std::string>& CPromiseResult::TextRejected()
{
    static const std::shared_ptr<const std::string> s_sp = std::make_shared<const std::string>("未指定原因");
    return s_sp;
}

inline const std::shared_ptr<const std::string>& CPromiseResult::TextStopped()
{
    static const std::shared_ptr<const std::string> s_sp = std::make_shared<const std::string>("执行器已停");
    return s_sp;
}

inline const std::shared_ptr<const std::string>& CPromiseResult::TextException()
{
    static const std::shared_ptr<const std::string> s_sp = std::make_shared<const std::string>("处理器异常");
    return s_sp;
}

}  // namespace async
}  // namespace common
