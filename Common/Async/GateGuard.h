#pragma once

// ====================================================================
// 层体「自请过门」：让层体自己声明「我必须在读 / 写槽位里跑」（`ASYNC_GATE`）
//
// 背景：层的类别通常写在挂层处（`.Then(handler, TaskKind::kWrite, ASYNC_LOC)`）。同一个处理器
// 函数常被多处复用，写死在调用点要么过严（读流程白独占）、要么过松（碰了状态却没保护）。
// 本机制把「数据契约」的声明放回**函数体里**（谁碰谁声明），调用点只表达调度意图：
//
//   common::async::CPromiseResult StepRejectIfAbsent(const std::shared_ptr<CUserOpContext>& spCtx)
//   {
//       ASYNC_GATE_WRITE();   // 必须第一条语句：本函数体要在写槽位里跑
//       ...
//   }
//
// 挂起方式（无异常、无新状态、无额外分配）：
//   ① 未持有目标槽位 → 写进本次调用的作用域（TLS）并**返回占位结果**（`Resolve()`：空指针，
//      零分配）；层运行器看到挂起标记后**不 settle**，而是把「再跑一遍本层」按请求类别投递进读写门；
//   ② 门内跑起来时任务帧已匹配 → `EnterGate` 返回「继续」，函数体照常执行 ——
//      **「重入时跳过本行」不需要任何标志位**，靠帧匹配天然成立；
//   ③ 本层在重入完成前不 settle → 下游层不会跑，链序天然保持。
//
// 成本：已在同类槽位（常态）→ 1 次 TLS 取链 + 1 次指针比较，**零分配、零投递**；
//       需要升级类别 → 每层最多一次额外过门投递（重入运行器的一份拷贝 + 任务入队）。
//
// 纪律（文档写明，代码里也会诊断 / 断言）：
//   - 必须是**函数体第一条语句**（挂起时第一次只执行到它，之前的语句会白跑一次）；
//   - 只适用于返回 `CPromiseResult` 的**层体**（then / catch / finally / 首层处理器）；
//     协程体、`ThenPromise` 工厂、`OnSettled` 通知、`Post` 任务里没有层体作用域（会诊断）。
//   - **一个层体只允许出现一次**（编译期检查）：宏里带固定标签 `ASYNC_GATE_ONCE_PER_FUNCTION`，
//     同一函数（含嵌套块）里第二次使用 → `duplicate label` 编译错误（`.tools/check_async_gate_once.sh`
//     守着这条性质）。为什么这么严：两种类别会在读 / 写槽位之间**来回换档重入**（挂起 → 重入 → 又
//     挂起）；确实需要两种类别，请**拆成两层**，每层各自声明自己的类别。
//   - 于是**一次运行最多挂起一次**（挂起后重入的帧必然匹配，那一行落穿）—— 这也是层运行器的兜底判据：
//     第二次挂起 = 漏网的换档写法（辅助函数 / lambda 里再声明门、绕过宏直接请求 `detail::EnterGate`），
//     立即收口成有界失败（诊断 + 本层拒绝），不让它在读 / 写槽位之间无限换档。
//
// 与挂层处类别的关系：调用点类别负责**首次准入**（决定排队与首层的进入方式），本宏负责
// **数据契约**；两者一致时宏是零成本 no-op，不一致时才发生「挂起 + 重入」。
//
// 只收一个参数（类别）就够了：执行器不用传（就是「本链执行器」= 本层所属模块），
// 函数名 / 上下文也不用传（重入由层运行器做 —— 处理器与共享上下文都在它手里）。
// ====================================================================

#include "Assert.h"
#include "Async/Diagnostics.h"
#include "Async/ReadWriteGate.h"

namespace common::async {
namespace detail {

/// @brief 诊断：`ASYNC_GATE` 用在了没有层体作用域的地方（协程体 / 工厂 / 通知 / `Post` 任务）。
constexpr const char* kDiagGateOutsideLayer =
    "ASYNC_GATE 只能用在层体里（then / catch / finally / 首层处理器）：此处既不是层体，"
    "也没有持有同类门槽位 —— 请求被忽略（该处未受读写门保护）";

/// @brief 诊断：层体请求过门，但本层执行器没有读写门（理论分支）。
constexpr const char* kDiagGateNoGate = "ASYNC_GATE：本层执行器没有读写门，无法过门（照常执行，未受保护）";

/// @brief 诊断：同一层体在一次运行里**挂了第二次** —— 兜底分支（正常写法在编译期就被拦下了）。
///
/// 正常路径一次运行最多挂起一次：宏里带固定标签，同一函数里写不了第二处（`duplicate label`），
/// 而挂起后的重入帧必然匹配 → 那一行落穿。能第二次挂起的只有两种漏网写法：
///  - 层体里调用**另一个带宏的函数**（辅助 lambda / 辅助函数有自己的函数作用域，标签不冲突，
///    但挂起作用域与本层共享）→ 外层要写、内层要读，换档来回；
///  - 绕过宏直接请求 `detail::EnterGate`。
/// 两者都收口成**有界失败**（诊断 + 本层拒绝）：不这么做就会在门里无限换档（链永不落定）。
constexpr const char* kDiagGateSuspendedTwice =
    "ASYNC_GATE 重复挂起：同一层体一次运行里只能挂起一次（辅助函数里再声明门 / 绕过宏直接请求会让本层"
    "既读又写），本层已收口 —— 拆成两层即可";

/// @brief 本次「层体调用」的作用域（TLS 链；由层运行器在进层体前压栈）。
///
/// 只记两件事：本层的门（判定「我是不是已经在目标槽位里」）与层体请求的类别（挂起时有效）。
/// 内联级联会自然嵌套（`pPrev` 还原外层）。
///
/// 写法与 `CTaskFrame`（读写门任务帧）同款：**构造压栈、析构弹栈**，`Top()` 取栈顶。
struct CGateCallScope
{
    const CReadWriteGate* pGate;  ///< 本层的门（执行器没有门时为 nullptr）。
    TaskKind eRequested;          ///< 层体请求的类别（挂起时有效）。
    bool bSuspended;              ///< 是否已登记挂起（此时函数返回的是占位结果，不可用）。
    CGateCallScope* pPrev;        ///< 外层作用域（内联级联）。

    /// @brief 压栈（TLS 顶 = 本作用域）。
    ///
    /// @param pGateIn 本层的门（可为 nullptr：执行器没有门）。
    explicit CGateCallScope(const CReadWriteGate* pGateIn)
        : pGate(pGateIn), eRequested(TaskKind::kDirect), bSuspended(false), pPrev(Top())
    {
        Top() = this;
    }

    /// @brief 弹栈（还原外层）。
    ~CGateCallScope()
    {
        Top() = pPrev;
    }

    CGateCallScope(const CGateCallScope&) = delete;
    CGateCallScope& operator=(const CGateCallScope&) = delete;

    /// @brief 当前线程的作用域栈顶（不在层体里时为 nullptr）。
    ///
    /// @return 栈顶引用（可读可写：构造 / 析构时改写它）。
    static CGateCallScope*& Top()
    {
        static thread_local CGateCallScope* s_pTop = nullptr;
        return s_pTop;
    }
};

/// @brief `EnterGate` 的判定结果。
enum class EGateEnter
{
    kContinue,  ///< 已在目标槽位（或直投 / 没有门）：继续执行函数体。
    kReenter,   ///< 已登记挂起：调用方必须**立即返回占位结果**（运行器会重入本层）。
};

/// @brief 当前线程是否已在「该门的该类槽位」里跑（就是任务帧匹配，零成本）。
///
/// @param pGate 目标门。
/// @param eKind 目标类别。
/// @return true 已在（含内联级联下来的情况）。
inline bool InGateSlot(const CReadWriteGate* pGate, TaskKind eKind)
{
    const CTaskFrame* pFrame = CTaskFrame::Top();
    return pFrame != nullptr && pFrame->pGate == pGate && pFrame->eKind == eKind;
}

/// @brief 请求「本函数体在 eKind 槽位里执行」（`ASYNC_GATE` 的实现）。
///
/// @param eKind 请求的类别（`kRead` 可并发 / `kWrite` 独占 / `kDirect` 不过门 = 直接继续）。
/// @return 见 `EGateEnter`：`kContinue` 直接往下执行；`kReenter` 立即返回占位结果。
inline EGateEnter EnterGate(TaskKind eKind)
{
    // ① 直投：不过门，无需请求。
    if (eKind == TaskKind::kDirect)
    {
        return EGateEnter::kContinue;
    }

    CGateCallScope* pScope = CGateCallScope::Top();
    if (pScope == nullptr)
    {
        // 不在层体里（Post 任务 / 协程体 / 工厂 / 通知 / 模块公开异步函数）：没有「重入」可言。
        // 已持同类槽位时照常执行；否则只报告（不改变行为 —— 与 `kDiagAwaitRisk` 同口径）。
        // 注：宏形态只写得进返回 `CPromiseResult` 的层体，走到这里多半是函数形态的误用。
        const CTaskFrame* pFrame = CTaskFrame::Top();
        if (pFrame == nullptr || pFrame->eKind != eKind)
        {
            ReportDiagnostic(kDiagGateOutsideLayer);
        }
        return EGateEnter::kContinue;
    }

    // ② 执行器没有门（理论分支）：无从过门，照常执行并报告。
    if (pScope->pGate == nullptr)
    {
        ReportDiagnostic(kDiagGateNoGate);
        return EGateEnter::kContinue;
    }

    // ③ 已在目标槽位（含「挂起后重入跑起来」的那一次）：继续执行函数体 —— 这就是「跳过本行」。
    if (InGateSlot(pScope->pGate, eKind))
    {
        return EGateEnter::kContinue;
    }

    // ④ 登记挂起：层体立即返回占位结果，由层运行器把「再跑一遍本层」投递进读写门。
    pScope->eRequested = eKind;
    pScope->bSuspended = true;
    return EGateEnter::kReenter;
}

}  // namespace detail
}  // namespace common::async

/// @brief 声明本层体需要「本链执行器的 kind 槽位」（必须放在函数体第一条语句）。
///
/// **一个函数只允许出现一次**（编译期检查）：宏里带固定标签 `ASYNC_GATE_ONCE_PER_FUNCTION`，同一函数里
/// 第二次出现 → `duplicate label` 编译错误（`.tools/check_async_gate_once.sh` 守着这条不变）。
/// 已在目标槽位 → 零成本落穿；否则挂起本层并过门重入（重入后自动跳过本行）。
/// 用法：`ASYNC_GATE_READ();` / `ASYNC_GATE_WRITE();` / `ASYNC_GATE(kWrite);`
#define ASYNC_GATE(kind)                                                                                              \
    ASYNC_GATE_ONCE_PER_FUNCTION: /* ← 每个函数只允许一处：第二处 = duplicate label 编译错误 */    \
    do                                                                                                                \
    {                                                                                                                 \
        if (false)                                                                                                    \
        {                                                                                                             \
            goto ASYNC_GATE_ONCE_PER_FUNCTION; /* 引用一下标签：避开 -Wunused-label */                       \
        }                                                                                                             \
        if (::common::async::detail::EGateEnter::kReenter ==                                                          \
            ::common::async::detail::EnterGate(::common::async::TaskKind::kind))                                      \
        {                                                                                                             \
            return ::common::async::CPromiseResult::Resolve(); /* 占位：运行器见挂起标记后重入本层 */ \
        }                                                                                                             \
    } while (0)

/// @brief `ASYNC_GATE(kRead)` 的简写：本层体与其它读任务并发（只读，不改模块状态）。
#define ASYNC_GATE_READ() ASYNC_GATE(kRead)

/// @brief `ASYNC_GATE(kWrite)` 的简写：本层体独占进入本模块（会改模块状态）。
#define ASYNC_GATE_WRITE() ASYNC_GATE(kWrite)
