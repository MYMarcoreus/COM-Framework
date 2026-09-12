#pragma once

#include <functional>
#include <string>

#include "Async/PromiseTypes.h"
#include "Async/SourceLoc.h"

// ====================================================================
// 异步调用链（trace）—— 「在异步函数内部看到自己处在哪条链上」
//
// 同步代码里 `backtrace()` 就能看到调用链，因为栈是连续的；异步里层与层之间是投递 /
// 回调，栈早就断了 —— 所以这里给的不是「栈回溯」，而是**因果链**：本层 ← 谁挂的它 ← …
//
// 用法（在任意层处理器内部）：
// @code
// static common::async::CPromiseResult StepVerify(common::async::CPromiseResult upResult,
//                                                 const std::shared_ptr<Ctx>& spCtx)
// {
//     common::async::DumpLayerChain();                  // 排障：这层是谁挂上来的
//     common::async::VisitLayerChain([](const common::async::CLayerInfo& info)
//     {
//         Log("depth=%d mode=%d %s (%s:%d)", info.nDepth, info.eMode,
//             info.loc.szFunction, info.loc.szFile, info.loc.nLine);
//     });
//     ...
// }
// @endcode
//
// 机制（**不改处理器签名、不增加堆分配**）：
//  - 跑层时在 thread_local 上压一个「当前层」帧（`detail::CCurrentLayerFrame`，帧活在任务体的栈上）；
//  - 每层状态记下「挂在哪一层之下」（强引用，落定时释放 → 链完整、无环、持有量有界）；
//  - `VisitLayerChain` = 当前层 + 顺上游指针一直走到链根。
//
// 开关：与注册点 `ASYNC_LOC` 同一个（调试构建 `ASYNC_DEBUG_TRACE`，见 SourceLoc.h）——
// 发布构建下本文件所有接口都是空操作（`CurrentLayer()` 返回 nullptr），零开销。
//
// 边界（异步的固有性质，不是实现偷懒）：
//  - 只能看到「**当前层 + 上游**」：下游（还没跑的层）是运行期才挂的，看不到；
//  - 分叉（同层多个 Then）→ 树；组合器（WhenAll 一族）→ 多父一子（严格说是 DAG）；
//  - 跨模块：止于本链（子链是 L2 的事，见 docs/common/async-impl.md §14）；
//  - 通知（`OnSettled`）不是层：它不新建层帧 —— 投递送达时看不到层；
//    就地送达时看到的是**触发它的那一层**（帧还在栈上）。
// ====================================================================

namespace common {
namespace async {

namespace detail {
class CPromiseState;  // 前置声明（帧只持有它的常量指针）。
}  // namespace detail

/// @brief 链上的一层（`CurrentLayer` / `VisitLayerChain` 的结果）。
struct CLayerInfo
{
    CSourceLoc loc;             ///< 注册点（发布构建为空；调试构建 = `ASYNC_LOC` 传入的位置）。
    detail::HandlerMode eMode;  ///< then / catch / finally。
    int nDepth;                 ///< 距当前层几跳（0 = 正在执行的那一层）。
    bool bCurrent;              ///< 是不是正在执行的那一层。

    /// @brief 默认：空信息。
    CLayerInfo() : loc(), eMode(detail::kModeThen), nDepth(0), bCurrent(false)
    {}
};

/// @brief 当前正在执行的那一层（不在任何层里 → nullptr）。
///
/// 「正在执行」= 某个层处理器的函数体里（内联级联时是最内层那个）。
/// **返回值指向 thread_local 存储**：下次调用会被覆盖，要留住请自行拷贝。
const CLayerInfo* CurrentLayer();

/// @brief 遍历「当前层 → 上游链」（由近到远），一直走到链根。
///
/// @param fnVisit 访问器（每层调用一次）。
/// @return true = 确实在层里且至少访问了一层；false = 不在层里（此时不调用访问器）。
bool VisitLayerChain(const std::function<void(const CLayerInfo&)>& fnVisit);

/// @brief 把「当前层 → 上游链」拼成一行（便于写日志 / 测试断言）。
///
/// @return 形如 `#0 then StepC (trace.cpp:42) <- #1 then StepB (trace.cpp:38)`；
///         不在层里返回空串。
std::string DescribeLayerChain();

/// @brief 把「当前层 → 上游链」打印到 stderr（调试构建；发布构建空操作）。
void DumpLayerChain();

namespace detail {

/// @brief 当前层帧（thread_local 栈；帧对象活在跑层任务体的栈上 → 零分配）。
///
/// 内联级联会**嵌套跑层**（外层处理器里内联跑下一层），所以是**栈语义**：
/// 构造压栈、析构弹栈，必须成对（RAII 保证异常路径也不漏）。
class CCurrentLayerFrame
{
public:
    /// @brief 压栈（记录「当前层」）。
    ///
    /// @param pLayer 本帧对应的层。
    explicit CCurrentLayerFrame(const CPromiseState* pLayer);

    /// @brief 弹栈（恢复外层帧）。
    ~CCurrentLayerFrame();

    CCurrentLayerFrame(const CCurrentLayerFrame&) = delete;
    CCurrentLayerFrame& operator=(const CCurrentLayerFrame&) = delete;

    /// @brief 栈顶帧（nullptr = 不在任何层里）。
    static const CCurrentLayerFrame* Top();

    /// @brief 本帧对应的层。
    const CPromiseState* Layer() const
    {
        return m_pLayer;
    }

    /// @brief 外层帧。
    const CCurrentLayerFrame* Prev() const
    {
        return m_pPrev;
    }

private:
    const CPromiseState* m_pLayer;      ///< 本帧的层。
    const CCurrentLayerFrame* m_pPrev;  ///< 外层帧（thread_local 栈）。
};

}  // namespace detail
}  // namespace async
}  // namespace common
