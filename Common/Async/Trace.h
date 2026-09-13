#pragma once

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
//     common::async::DumpLayerChain();                  // 排障：这层是谁挂上来的（多行块，一层一行）
//     common::async::VisitLayerChain([](const common::async::CLayerInfo& info)
//     {
//         Log("%s", common::async::DescribeLayer(info).c_str());   // 自己控制格式：一行富信息
//     });
//     ...
// }
// @endcode
//
// --------------------------------------------------------------------
// **只在调试构建存在**：整套东西与注册点 `ASYNC_LOC` 同一个开关
// （`ASYNC_DEBUG_TRACE`，见 SourceLoc.h —— 判定 = 未定义 `NDEBUG` 且未开优化）。
//
// 发布构建下这个头文件**什么也不提供**：`CLayerInfo` 与所有函数整段不参与编译
// （不是「空操作版本」—— 拿不到类型、也调不到函数），所以要写 trace 相关代码的地方
// 得自己包住：
// @code
// #if defined(ASYNC_DEBUG_TRACE)
//     common::async::DumpLayerChain();
// #endif
// @endcode
// 这样「调试专用」在编译期就是清楚的，也不会在发布构建里留下一堆返回空值的假接口。
// --------------------------------------------------------------------
//
// 机制（**不改处理器签名**）：
//  - 跑层时在 thread_local 上压一个「当前层」帧（`detail::CCurrentLayerFrame`，帧活在任务体的栈上）；
//  - 每层状态记下「挂在哪一层之下」（**强引用，注册时设一次、之后只读**：只要下游还活着，
//    上游就不会被释放 → 链总是完整的；边只指上游，所以无环。代价（仅调试构建）：
//    持有尾层句柄会把整条前缀留住）；
//  - `VisitLayerChain` = 当前层 + 顺上游指针一直走到链根。
//
// 子链 → 父链（子链不是孤链）：**起链时正在跑的那一层**会被记成新链「链根」的父层。
// 内层链（`ThenPromise`）、跨模块子链（`ThenBridge`）、组合器聚合链、层里 fire-and-forget
// 起的小链，因此都能从子链里一路追回父链 —— 一棵树 / DAG。
// 例外：工厂返回的若是**别处早已建好**的链，它保留原来的归属（不重挂）；
//       协程起的子链挂在「启动协程的那一层」（`CoStart` 处）。
//
// 边界（异步的固有性质，不是实现偷懒）：
//  - 只能看到「**当前层 + 上游**」：下游（还没跑的层）是运行期才挂的，看不到；
//  - 分叉（同层多个 Then）→ 树；组合器（WhenAll 一族）→ 多父一子（严格说是 DAG）；
//  - 通知（`OnSettled`）不是层：它不新建层帧 —— 投递送达时看不到层；
//    就地送达时看到的是**触发它的那一层**（帧还在栈上）。
// ====================================================================

#if defined(ASYNC_DEBUG_TRACE)

    #include <chrono>
    #include <functional>
    #include <string>
    #include <thread>

    #include "Async/PromiseTypes.h"

namespace common {
namespace async {

namespace detail {
class CPromiseState;  // 前置声明（帧/记录里持它的强引用：起链时要把「父层」交给新链）。

/// @brief 下一个「层号」（全局递增；日志里对同一层反复对账用）。
unsigned NextLayerId();

/// @brief 下一个「链号」（全局递增；链号在**链根**创建时分配，追加的层继承）。
unsigned NextChainId();
}  // namespace detail

/// @brief 一层的 trace 信息 —— **既是层状态里存的记录，也是遍历时给调用方的视图**。
///
/// 一份数据一个类型：上半部分是「层自己的记录」（创建 / 开跑 / 落定时各写一次，之后只读），
/// 遍历时原样带出来；下半部分是「本次遍历算出来的视图字段」（层状态里不存它们）。
/// 所以不需要「内部记录 + 对外视图」两份结构，也不用在遍历时逐字段搬运。
struct CLayerInfo
{
    //================ 层状态里存的（写一次，之后只读） ================

    CSourceLoc loc;                                   ///< 注册点（`ASYNC_LOC` 传入的位置）。
    detail::HandlerMode eMode;                        ///< then / catch / finally。
    std::shared_ptr<detail::CPromiseState> upstream;  ///< 上游层（谁挂的它；链根 → 空）。

    /// 本层**实际跑在哪个执行器**上（执行器名；未命名 / 直接跑在当前线程的层 → 空）。
    ///
    /// 链可能在**几条链 / 几个执行器**之间接力（子链跑在别的模块的执行器上），所以这是**逐层**的属性。存 `shared_ptr`
    /// 而不是裸指针：执行器析构后已起的链还会跑完，名字得跟着活（名字串在执行器构造时
    /// 分配一次，各层共享）。
    std::shared_ptr<const std::string> spExecName;

    unsigned nLayerId;                               ///< 层号（全局唯一，按创建顺序递增）。
    unsigned nChainId;                               ///< 链号（链根创建时分配；子链与父链不同号）。
    bool bChainRoot;                                 ///< 是不是「它那条链」的链根。
    bool bSubChain;                                  ///< 链根且起链时挂在别的层下面（= 子链）。
    std::thread::id tid;                             ///< 实际跑在哪条线程上（还没跑过 → 默认 id）。
    long long nSelfMs;                               ///< 本层处理器耗时（落定前写一次）。
    std::chrono::steady_clock::time_point tCreated;  ///< 层状态创建时刻（算年龄用）。

    //================ 遍历时填的视图字段 ================

    int nDepth;        ///< 距当前层几跳（0 = 正在执行的那一层）。
    bool bCurrent;     ///< 是不是正在执行的那一层。
    long long nAgeMs;  ///< 本层状态创建 → 现在（链根上 = 整条链的年龄）。
    bool bSettled;     ///< 是否已落定（正在跑的当前层恒为 false）。
    bool bFulfilled;   ///< 落定结果是否兑现（`bSettled` 为 true 时有意义）。
    int nCode;         ///< 落定结果码（`bSettled` 为 true 时有意义）。

    /// @brief 默认：空信息（创建时刻取当下；视图字段为 0）。
    CLayerInfo()
        : loc(),
          eMode(detail::kModeThen),
          upstream(),
          spExecName(),
          nLayerId(0),
          nChainId(0),
          bChainRoot(false),
          bSubChain(false),
          tid(),
          nSelfMs(0),
          tCreated(std::chrono::steady_clock::now()),
          nDepth(0),
          bCurrent(false),
          nAgeMs(0),
          bSettled(false),
          bFulfilled(false),
          nCode(0)
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
/// @return 形如 `#0 then StepC (trace.cpp:42) <- #1 then StepB (trace.cpp:38)`
///         —— 只有模式 + 注册点，**不包含**执行器 / 层号 / 耗时 / 结果；
///         要那此信息用 `DescribeLayerChainBlock()`（每层一行）或 `DescribeLayer()`。
///         不在层里返回空串。
std::string DescribeLayerChain();

/// @brief 把一层拼成**一行富信息**（注册点 + 层号/链号 + 线程 + 耗时 + 结果）。
///
/// @param info 一层（`CurrentLayer()` / `VisitLayerChain()` 给的）。
/// @return 一行。
std::string DescribeLayer(const CLayerInfo& info);

/// @brief `DescribeLayerChainBlock()` / `DumpLayerChain()` 默认最多打印多少层。
///
/// 深链（数百层）全部打印没有读的价值，所以默认「头尾各几层 + 中间省略」；
/// 传 0 表示不限（测试断言 / 导出用）。
const int kDumpMaxLayers = 24;

/// @brief 把「当前层 → 上游链」拼成**多行的排障文本块**（头行 + 每层一行）。
///
/// 与 `DescribeLayerChain()`（一行压缩链）的区别：这里是「能直接看的排障视图」——
/// 每层用 `DescribeLayer()`（含执行器 / 层号 / 耗时 / 结果 / 当前层标记），
/// 开头一行给总数。层数超过 `nMaxLayers` 时只打印头尾、中间用一行省略标记（`nMaxLayers <= 0` = 全部）。
///
/// 与 `DumpLayerChain()` 的区别：这里返回字符串（可写自己的 logger / 断言内容），
/// 不进 stderr。
///
/// @param nMaxLayers 最多打印多少层（<= 0 = 不限；默认 `kDumpMaxLayers`）。
/// @return 多行文本（末尾带换行）；不在层里返回空串。
std::string DescribeLayerChainBlock(int nMaxLayers = kDumpMaxLayers);

/// @brief 把 `DescribeLayerChainBlock()` 打印到 stderr（**整块一次写出**：多线程同时 dump 不会互相插花）。
///
/// 不在层里也会打一行提示（而不是什么都不打）—— 「dump 了但没输出」最容易让人误以为接口没生效。
///
/// @param nMaxLayers 最多打印多少层（<= 0 = 不限；默认 `kDumpMaxLayers`）。
void DumpLayerChain(int nMaxLayers = kDumpMaxLayers);

namespace detail {

/// @brief 起链时要挂在下面的「父层」。
///
/// 优先级（见 `CChainAdopterScope`）：
///  1. 显式作用域（工厂在跑时套上的那一层）→ 子链挂在「正在等子链的那一层」下面；
///  2. 当前正在跑的层（帧栈顶）→ 层里 fire-and-forget 起的链挂在那一层下面；
///  3. 都没有（层外起的链）→ 空（链根就是链根）。
///
/// 起链入口把它交给新链的**链根**当上游（`CPromise::StartChain` / `NewFromHandle`）——
/// 这就是「子链能一路追回父链」的那条边。
///
/// @return 父层（没有 → 空 shared_ptr）。
std::shared_ptr<CPromiseState> CurrentLayerState();

/// @brief 起链父层的显式作用域（RAII）—— 作用域内起的链，链根挂在指定那一层下面。
///
/// 两个使用场合：
///  - `Adopt`（`ThenPromise`）/ `ThenBridge` 的工厂：工厂是在**上游层**的 settle 路径里跑的，
///    只靠帧栈顶会落回上游层、链上就看不到 `ThenPromise` 那一层 —— 这里显式指定「正在等子链的本层」；
///  - 协程的 `NewPromise`：把父层钉成「启动协程的那一层」（与恢复时机无关，所以是确定的）。
class CChainAdopterScope
{
public:
    /// @brief 在作用域内，起链父层固定为 spAdopter（空 = 不指定，落回帧栈顶）。
    ///
    /// @param spAdopter 父层。
    explicit CChainAdopterScope(const std::shared_ptr<CPromiseState>& spAdopter);

    /// @brief 退出作用域（恢复外层）。
    ~CChainAdopterScope();

    CChainAdopterScope(const CChainAdopterScope&) = delete;
    CChainAdopterScope& operator=(const CChainAdopterScope&) = delete;

    /// 作用域节点（**内部用**：活在栈上，thread_local 只存栈顶指针；公开是为了让实现里能声明 TLS）。
    struct CNode
    {
        std::shared_ptr<CPromiseState> spAdopter;  ///< 本作用域的父层。
        const CNode* pPrev;                        ///< 外层节点。
    };

private:
    CNode m_node;  ///< 本作用域的节点（构造时压栈、析构时弹栈）。
};

/// @brief 当前层帧（thread_local 栈；帧对象活在跑层任务体的栈上 → 零分配）。
///
/// 内联级联会**嵌套跑层**（外层处理器里内联跑下一层），所以是**栈语义**：
/// 构造压栈、析构弹栈，必须成对（RAII 保证异常路径也不漏）。
class CCurrentLayerFrame
{
public:
    /// @brief 压栈（记录「当前层」并记下它的线程、执行器与开始时刻）。
    ///
    /// @param spLayer 本帧对应的层（持强引用：`LayerState()` 要把「父层」交给新起的链）。
    explicit CCurrentLayerFrame(const std::shared_ptr<CPromiseState>& spLayer);

    /// @brief 弹栈（恢复外层帧）。
    ~CCurrentLayerFrame();

    CCurrentLayerFrame(const CCurrentLayerFrame&) = delete;
    CCurrentLayerFrame& operator=(const CCurrentLayerFrame&) = delete;

    /// @brief 栈顶帧（nullptr = 不在任何层里）。
    static const CCurrentLayerFrame* Top();

    /// @brief 本帧对应的层。
    const CPromiseState* Layer() const
    {
        return m_spLayer.get();
    }

    /// @brief 本帧对应的层（强引用）—— 起链时当「父层」用。
    const std::shared_ptr<CPromiseState>& LayerState() const
    {
        return m_spLayer;
    }

    /// @brief 本层从开始跑到现在的毫秒数（`CurrentLayer()` 报「已跑多久」用）。
    long long ElapsedMs() const;

    /// @brief 外层帧。
    const CCurrentLayerFrame* Prev() const
    {
        return m_pPrev;
    }

private:
    std::shared_ptr<CPromiseState> m_spLayer;  ///< 本帧的层（强引用：它正在跑，本帧作用域内必然存活）。
    const CCurrentLayerFrame* m_pPrev;         ///< 外层帧（thread_local 栈）。
    std::chrono::steady_clock::time_point m_t0;  ///< 本层开始跑的时刻（算耗时用）。
};

}  // namespace detail
}  // namespace async
}  // namespace common

#endif  // defined(ASYNC_DEBUG_TRACE)
