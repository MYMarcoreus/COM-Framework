#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Async/Diagnostics.h"
#include "Async/PromiseResult.h"
#include "Async/PromiseTypes.h"
#include "Async/ReadWriteGate.h"
#include "Async/SourceLoc.h"
#include "Thread/ThreadPool.h"

// ====================================================================
// 异步执行器（调度层）
//
// 职责：持有工作线程池，提供「投递执行」与「起链入口」—— 起 promise / 起协程
// （`NewPromise` / `CoStart` / 组合器）以及把多个子 promise 汇成一条聚合链的
// 「并行组合」（`WhenAll` 一族）。
// 不做链式编排（`Then` 一族见 Promise.h）、不做顺序化（见 Coroutine/Coroutine.h）。
//
// 为什么要有这个类（「JS 里没有对应物」）：JS 的调度是「隐式」的 —— 由宿主事件循环 +
// 微任务队列接管，「谁跑回调」根本不是 API 的一部分。C++ 没有宿主循环，于是三件事必须
// 由一个显式对象回答，它们就是本类的全部职责：
//   - 任务投到哪条线程：`Post` / `NewPromise` / `CoStart`；
//   - 一条链的层在哪条线程上跑：「本链执行器」（同线程内联级联；跨执行器/跨模块返回则投递回本链）。
//     （曾有过「逐层指定线程」的 `ThenInline` / `ThenOn`，2026-09-13 已移除 —— 理由见
//      docs/common/async-impl.md §8.1「历史」：就地层的落点取决于上游何时落定，一层可能三种落点。）
//   - 执行器停了以后怎么办：`Stop` + 句柄加固（新投递以框架侧拒绝「执行器已停」收口）。
// 模块内读写（读写门，见 Async/ReadWriteGate.h）：
//   执行器内部组合一个读写门，每条投递都带「类别」：读任务（kRead）可并发、写任务（kWrite）独占。
//   于是「一个模块 = 一个执行器」时，模块数据在「读任务只读、写任务写完」的规约下不再需要自己的锁
//   （锁在异步里跨不了挂起点，按任务粒度各自加锁又会占住工作线程且没有公平性）。
//   类别：**所有异步入口都要显式给出**（`Post` / `NewPromise` / `Then` 一族 / `CoStart`），
//   本框架不提供「默认读 / 默认写」—— 默认值会让「忘记声明的读」变成静默并发问题。
//   唯一的例外是组合器（`WhenAll` 一族）：它们**没有类别参数** —— 聚合层是「框架簿记层」
//   （只被 settle，不跑业务代码、不碰模块状态），本就不该占槽位；子链 / 后续层各自带自己的类别。
//   第三类 `kDirect`（直投）：不入队、不占槽位、不过门 —— 代码里必须看得见“这不需要门”，
//   于是它才是显式的少数派，而不是“忘了写”的默认值（契约：不得访问受门保护的数据）。
//   就地级联同样受门约束（判定在 `detail::ShouldInline` → `CReadWriteGate::CanRunInline`）：
//   只有「本线程正跑着本门同类任务 + 无人在排队」才就地，否则入队 —— 前者保互斥，后者保公平；
//   直投层不查门（它不占槽位），在本执行器线程上就接着跑。
//
// 调度对照（各自生态里的同类物）：
//   CAsyncExecutor  ≈ Java `Executor` / C# `TaskScheduler` / Asio `io_context` / dispatch_queue
//   exec.Post(fn)   ≈ Asio `io_context::post` / Java `Executor.execute`
//     （本框架的 Post 多一个必填的类别参数：读可并发 / 写独占 / 直投不过门。）
//   exec.CoStart<T> ≈ C# `Task.Run`（续跑线程由调度器决定）
//   而 JS 这边：`setTimeout(fn, 0)` 是「宿主 API」（不在 Promise 里），`queueMicrotask(fn)`
//   才是微任务投递 —— 两者都不可控线程，因此不能与 `Post` 画等号。
//
// 用法：
//   common::async::CAsyncExecutor exec(4);
//   exec.Start();
//   exec.Post(TaskKind::kWrite, []() { /* 无返回值任务 */ });
//   auto p = exec.NewPromise(spCtx, StepLoad, TaskKind::kWrite).Then(StepSave, TaskKind::kWrite);
//   exec.Stop();
//
// 生命周期：执行器析构会停止线程池并等待已投递任务完成；promise / 协程通过
// 共享句柄引用线程池，执行器析构后已起动的 promise 仍安全跑完（新投递以
// 「执行器已停」（框架侧拒绝：执行器不可用）。
// ====================================================================

namespace common {
namespace async {

template <typename TContext>
class CPromise;  // 前置声明（NewPromise / WhenAll 返回 promise 句柄）。

template <typename TContext>
class CCoroutine;  // 前置声明（CoStart 返回协程句柄）。

namespace detail {

/// @brief 执行器侧诊断文案（集中一处：测试断言常量，而不是去匹配子串）。
constexpr const char* kDiagPostThrow = "exec.Post() 投递的任务抛出了异常（已兜住，未终止进程）";
constexpr const char* kDiagPostEmpty = "exec.Post(): 任务为空（未提交）";

/// @brief 执行器句柄（生命周期加固核心）。
///
/// promise / 协程持有本句柄：执行器析构后线程池对象仍存活（已投递任务跑完），
/// 新投递被 m_bStopped 拒绝并转为拒绝结果；读写门与线程池同寿命（模块的并发规则跟着句柄走）。
struct CExecutorHandle
{
    std::shared_ptr<common::thread::CThreadPool> m_pPool;  ///< 工作线程池。
    std::shared_ptr<CReadWriteGate> m_pGate;               ///< 读写门（模块内读并发 / 写独占）。
    std::atomic<bool> m_bStopped;                          ///< 是否已停止（拒绝新投递）。

    CExecutorHandle() : m_bStopped(false)
    {}
};

/// @brief 框架「簿记层」的类别（**= `kDirect`**）：组合器的聚合层、`ThenBridge` 内部那条「等子链」的链、
///        协程的完成标记层。
///
/// 这些层**只被 settle**（大多根本不投递）：不跑业务代码、不碰模块状态 —— 于是用 `kDirect`（不过门）是
/// 唯一诚实的选择：不占槽位、不受公平性约束（占一个读 / 写槽位只会无意义地阻塞模块里的真任务）。
///
/// **行为效果只有一处**：组合器「空集合」那条路径真的会投递一次 settle 任务（`StartChain` 的
/// 「起链即投递」），用簿记类别 = 它**不等门**（写者占着模块时也当场收口；`AsyncRw_GatherLayerBypassesGate`
/// 钉住）。其余用途（非空聚合层 / 内部簿记链 / 协程完成标记层）都不过门，那里的类别**只服务 trace**
/// （类别列显示「直」）。
constexpr TaskKind kKindBookkeeping = TaskKind::kDirect;

/// @brief 向执行器句柄「直投」任务（不过读写门）—— 只给「通知」这类必须送达的轻量任务用。
///
/// 通知受「保证送达」保护（执行器不可用时就地执行），不能去排队等槽位；业务任务请走
/// 带类别的重载（过门）。
///
/// @param pHandle 执行器句柄。
/// @param fnTask 任务函数（移动投递）。
/// @return true 投递成功；false 句柄不可用（空 / 已停止 / 线程池拒绝）。
inline bool PostToHandle(const std::shared_ptr<CExecutorHandle>& pHandle, std::function<void()> fnTask)
{
    if (pHandle == nullptr || pHandle->m_pPool == nullptr || pHandle->m_bStopped)
    {
        return false;
    }
    return pHandle->m_pPool->Submit(std::move(fnTask));
}

/// @brief 向执行器句柄「按类别投递」任务 —— 业务任务（层派发 / Post）走这条。
///
/// 两个分支：
///  - `kRead` / `kWrite`：过读写门（排队等槽位）；
///  - `kDirect`：直投线程池（不过门，随时可跑）。
///
/// @param pHandle 执行器句柄。
/// @param eKind 任务类别（读可并发 / 写独占 / 直投不过门）。
/// @param fnTask 任务函数（移动投递）。
/// @return true 已接受（可能已投递，也可能在门口排队）；false 句柄不可用（空 / 已停止 / 门已关闭）。
inline bool PostToHandle(const std::shared_ptr<CExecutorHandle>& pHandle, TaskKind eKind, std::function<void()> fnTask)
{
    if (eKind == TaskKind::kDirect)
    {
        return PostToHandle(pHandle, std::move(fnTask));  // 直投：与读写门无关。
    }

    if (pHandle == nullptr || pHandle->m_pGate == nullptr || pHandle->m_bStopped)
    {
        return false;
    }
    return pHandle->m_pGate->Submit(eKind, std::move(fnTask));
}

/// @brief 当前线程是否在某个执行器的工作线程上。
///
/// 层处理器只在「已经在本链执行器线程上」时就地内联，否则投递回本链执行器，
/// 从而保证「每一层都跑在它所属链的执行器线程上」。
///
/// @param pHandle 执行器句柄。
/// @return true 当前线程是该执行器的工作线程。
inline bool IsInExecutorThread(const std::shared_ptr<CExecutorHandle>& pHandle)
{
    return pHandle != nullptr && common::thread::CThreadPool::IsInPoolThread(pHandle->m_pPool.get());
}

/// @brief 级联内联深度（线程局部）：链逐层级联时最多连续内联多少层。
///
/// 超限则改为投递执行，避免超长链（数千层）在递归中爆栈。
const int kMaxInlineDepth = 64;

/// @brief 当前线程的级联内联深度。
inline int& InlineDepth()
{
    static thread_local int s_nInlineDepth = 0;
    return s_nInlineDepth;
}

/// @brief 内联深度的 RAII 守卫（构造 +1，析构 -1）。
///
/// 三处「就地内联」（层处理器 / 通知就地送达 / 协程续跑）都要配对增减；
/// 用守卫可以避免异常或提前 `return` 漏减（漏减会让后续层被多投递，且会累积）。
struct CInlineGuard
{
    CInlineGuard()
    {
        ++InlineDepth();
    }

    ~CInlineGuard()
    {
        --InlineDepth();
    }

    CInlineGuard(const CInlineGuard&) = delete;
    CInlineGuard& operator=(const CInlineGuard&) = delete;
};

/// @brief 是否应当「就地内联」（不投递、在当前线程上接着跑）。
///
/// 「就地还是投递」的唯一判定处（promise 的层派发与协程的续跑共用）：
///  - 已在本链执行器线程上：就地（省一次入队 + 保序）；
///  - 连续内联已达 `kMaxInlineDepth`：投递（防超长链爆栈）；
///  - `kDirect` 层：不查门（它不占槽位）—— 已在本执行器线程上就接着跑；
///  - 读写门放行（`CanRunInline`）：本线程须持着本门「同类」槽位（读任务里的读层 / 写任务里的写层），
///    且无人在排队 —— 换类别会自死锁（读里等写 = 等自己退出），插队会破坏公平；
///  - `bRequireIdle`：还要求线程池无积压（协程续跑用 —— 有积压时投递，保住并行度）。
///
/// @param pExec 本链执行器句柄。
/// @param eKind 本层类别（读 / 写 / 直投）。
/// @param bRequireIdle 是否要求线程池无积压才内联。
/// @return true = 调用方应当直接执行任务体；false = 应当投递。
inline bool ShouldInline(const std::shared_ptr<CExecutorHandle>& pExec, TaskKind eKind, bool bRequireIdle = false)
{
    // ① 线程亲和 + 内联深度。
    if (!IsInExecutorThread(pExec) || InlineDepth() >= kMaxInlineDepth)
    {
        return false;
    }

    // ② 直投层不过门：没有槽位要查，也没有排队要顾。
    if (eKind != TaskKind::kDirect && (pExec->m_pGate == nullptr || !pExec->m_pGate->CanRunInline(eKind)))
    {
        return false;
    }

    // ③ 协程续跑的额外条件：线程池无积压。
    return !bRequireIdle || (pExec->m_pPool != nullptr && pExec->m_pPool->PendingCount() == 0);
}

/// @brief 派发一个任务体：能就地就就地，否则按类别投递回本链执行器。
///
/// 判定见 `ShouldInline`（就地）与 `PostToHandle`（投递）；就地跑在外层任务已持有的槽位里
/// （不占位、不归还），投递则按类别过读写门排队。
///
/// @param pExec 本链执行器句柄。
/// @param eKind 本层类别（读 / 写 / 直投）。
/// @param fnTask 任务体（按值接收：就地执行或移动投递）。
/// @return true 已就地执行 / 已投递；false 执行器不可用（调用方以「执行器已停」收口本层）。
inline bool DispatchInlineOrPost(const std::shared_ptr<CExecutorHandle>& pExec, TaskKind eKind, std::function<void()> fnTask)
{
    if (ShouldInline(pExec, eKind))
    {
        CInlineGuard guard;  // 深度 +1 / -1 成对（异常 / 提前 return 也不漏减）。
        fnTask();
        return true;
    }
    return PostToHandle(pExec, eKind, std::move(fnTask));
}

}  // namespace detail

/// @brief 异步执行器：工作线程池 + 读写门 + 投递入口。
///
/// 非模板类；起 promise 通过模板成员 NewPromise 完成（上下文类型由参数推导）。
class CAsyncExecutor
{
public:
    //================ Lifecycle ================

    // 创建执行器（线程数默认 1）。
    explicit CAsyncExecutor(size_t nThreadCount = 1);

    // 创建「具名」执行器（线程数默认 1）。
    //
    // 名字只用于调试，但两处都很实用：
    //  ① 线程池的 worker 线程被命名为「<名字>-<序号>」—— gdb 的 `info threads` / htop 里直接
    //     能看出这条线程属于哪个执行器；
    //  ② 每层 trace 记下「这一层跑在哪个执行器上」，`DescribeLayer` 打印 `[名字]` —— 链跨模块
    //     接力（子链在别的模块的执行器上跑）时一眼能看出跑到谁家去了。
    //
    // 名字末尾会被线程名长度（15 字节）截断，所以给短一点（如 `main` / `db`）。
    //
    // @param strName 执行器名。
    // @param nThreadCount 工作线程数。
    CAsyncExecutor(const std::string& strName, size_t nThreadCount = 1);

    // 执行器名（空 = 未命名）。
    const std::string& Name() const
    {
        return m_strName;
    }

    // 不可拷贝（拷贝会共享线程池，Stop 相互影响）。
    CAsyncExecutor(const CAsyncExecutor&) = delete;
    CAsyncExecutor& operator=(const CAsyncExecutor&) = delete;

    // 销毁执行器（停止线程池并等待已投递任务完成）。
    ~CAsyncExecutor();

    // 启动工作线程（「停过」再启动会换一个新句柄：旧句柄上的链不会被新池接管）。
    //
    // 线程安全：**Start / Stop（含析构）须由所有者线程串行调用** —— 它们会关闭 / 重建
    // 执行器句柄，与其它线程并发 `Post` 不是线程安全的（运行期的 Post / 层派发是线程安全的）。
    bool Start();

    // 停止并等待任务完成（优雅关闭）。
    //
    // 顺序：关读写门（拒新）→ 标记停止 → 等门排空（已接受的过门任务跑完）→ 停线程池
    // （池按「队列排空才退出」停，所以直投任务与通知也一并排空，不丢）。
    void Stop();

    // 是否正在运行。
    bool IsRunning() const;

    // 是否已停止（停止后拒绝新投递）。
    bool IsStopped() const;

    //================ Post ================

    // 投递无返回值任务（fire-and-forget）：类别必填 —— 读可并发 / 写独占 / 直投不过门。
    //
    // 为什么没有默认值：类别决定模块内的互斥语义（“这条任务能不能和别的任务同时跑”），
    // 是行为契约而不是可选参数 —— 默认值会让“忘记声明的读”变成静默的并发问题。
    //
    // `kDirect` 适用于「自成一体的活」（自身线程安全、不碰模块状态）：它不排队、不占槽位，
    // 因而不受公平性保护（写任务扎堆时它照样能插进去跑）。
    bool Post(TaskKind eKind, std::function<void()> fnTask);

    //================ Chain ================

    // 起 promise（等价 JS `new Promise(executor)`）：创建 promise 并投递首层。
    //
    // 类别决定首层的投递路径（后续每层各自在 `Then` 一族里给类别）：
    // 读 / 写过门；`kDirect` 直投（不过门 —— 整链都不碰模块状态时用）。
    template <typename TContext>
    CPromise<TContext> NewPromise(const std::shared_ptr<TContext>& spContext, typename CPromise<TContext>::ThenHandler fnHandler,
        TaskKind eKind, const CSourceLoc& loc = CSourceLoc());

    // 起 promise（对齐 JS `new Promise((resolve, reject) => ...)`）：由起链回调内部的 resolve / reject 兑现。
    //
    // 启动时机与层体一致（就地 / 过门）：已在本门同类槽位里 → 就地同步跑（JS 语义）；
    // 否则（门外线程 / 别的模块的门 / 换类别）→ 按类别过门**投递后再跑** ——
    // 这条起链的类别因此是有效的：它就是「起链回调以什么身份进模块」。
    template <typename TContext>
    CPromise<TContext> NewPromise(const std::shared_ptr<TContext>& spContext,
        const typename CPromise<TContext>::ChainStarter& fnStarter, TaskKind eKind, const CSourceLoc& loc = CSourceLoc());

    //================ Combine ================

    // 组合器（对齐 JS `Promise.all`）：全部兑现才兑现；任一拒绝立即以该拒绝码拒绝。
    //
    // 组合器**没有类别参数**：聚合层不跑业务代码（只被 settle），子链与后续层各自带自己的类别。
    // 定义在 "Async/Combine.h"（组合器需要 `CPromise` 完整类型，本头只能前置声明它）。
    template <typename TContext, typename... TChild>
    CPromise<TContext> WhenAll(const std::shared_ptr<TContext>& spContext, const TChild&... child);

    // 组合器（对齐 JS `Promise.allSettled`）：全部落定即兑现（恒兑现）。
    template <typename TContext, typename... TChild>
    CPromise<TContext> WhenAllSettled(const std::shared_ptr<TContext>& spContext, const TChild&... child);

    // 组合器（对齐 JS `Promise.race`）：首个落定者定结果（兑现 / 拒绝皆可）。
    template <typename TContext, typename... TChild>
    CPromise<TContext> WhenRace(const std::shared_ptr<TContext>& spContext, const TChild&... child);

    // 组合器（对齐 JS `Promise.any`）：首个兑现者定结果；全部拒绝才失败。
    template <typename TContext, typename... TChild>
    CPromise<TContext> WhenAny(const std::shared_ptr<TContext>& spContext, const TChild&... child);

    //================ Coroutine ================

    // 创建并启动协程（类别必填：读可并发 / 写独占；投递首次 Resume）。
    template <typename TCoroutine, typename... TArgs>
    std::shared_ptr<TCoroutine> CoStart(TaskKind eKind, TArgs&&... args);

private:
    //================ Internal ================

    template <typename TContext>
    friend class CPromise;  // 取执行器句柄（起链 / 通知投递）。

    template <typename TContext>
    friend class CCoroutine;  // 取执行器句柄 + 空闲判定（子 promise 投递 / 内联续接）。

    // 当前线程是否本执行器的工作线程（框架内部用）。
    bool IsInExecutorThread() const
    {
        return detail::IsInExecutorThread(m_pHandle);
    }

    // 执行器句柄（promise / 协程持有，生命周期加固用）。
    auto Handle() const -> const std::shared_ptr<detail::CExecutorHandle>&
    {
        return m_pHandle;
    }

    // 新建句柄（连同线程池对象 + 读写门：都带上本执行器的名字）。
    auto MakeHandle() const -> std::shared_ptr<detail::CExecutorHandle>;

    // 投递实现（Post 的唯一实现）：包异常兜底后按类别过读写门。
    bool PostImpl(TaskKind eKind, std::function<void()> fnTask);

    // 注意声明顺序：成员按「声明序」初始化，而 `m_pHandle` 的构造（MakeHandle）要用到名字 ——
    // 所以 `m_strName` 必须声明在它前面。
    std::string m_strName;                               ///< 执行器名（调试用；空 = 未命名）。
    size_t m_nThreadCount;                               ///< 工作线程数。
    std::shared_ptr<detail::CExecutorHandle> m_pHandle;  ///< 执行器句柄（promise / 协程共享）。
};

//================ Combine ================

// 组合器（`exec.WhenAll` 一族：把多个子 promise 汇成一条「聚合链」）的**定义在 "Async/Combine.h"**。
//
// 为什么搬走（2026-09-26）：组合器要造 `CPromise` 实例，因此需要它的**完整类型**；而本头只能前置
// 声明 `CPromise`（`Promise.h` 反过来要 include 本头，不能成环）。留在本头时，这条依赖靠「模板
// 两段查找 + 调用方 TU 碰巧已 include Promise.h」兜住 —— 是个隐式契约；搬走后依赖关系明面化：
// 组合器头显式 include `Promise.h`，本头只留下面 4 个**成员声明**。
//
// 用组合器的 TU 请 include "Async/Combine.h"（语义要点与实现见该文件头注释）。

}  // namespace async
}  // namespace common
