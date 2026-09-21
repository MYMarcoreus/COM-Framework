#pragma once

#include <atomic>
#include <memory>
#include <utility>

#include "Assert.h"
#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "Async/PromiseTypes.h"
#include "Async/SourceLoc.h"

// ====================================================================
// CCoroutine —— 无栈协程（用顺序代码 await 多条 promise）
//
// 所在目录：Common/Coroutine/（顺序化是独立关注点，所以自己一个模块目录）；
// 依赖方向：Coroutine → Async（本文件 include AsyncExecutor.h / Promise.h），「反向无依赖」；
// 命名空间仍是 `common::async` —— 协程与 promise 共用同一套模型（同一个执行器句柄 +
// 同一份共享上下文），拆命名空间只会让调用方多写限定名。
//
// 定位：promise 负责「编排」（then / catch / finally 串起来，失败即停），
// 协程负责「顺序化」—— 用顺序代码 await 多条 promise，替代回调嵌套。
//
// 两者共用同一套模型：
//   - 协程持有一个共享上下文（std::shared_ptr<TContext>，与它起的子 promise 同一实例）；
//   - await 的对象是「promise」（含子协程 AsPromise() 暴露的 promise）；
//   - await 只告知兑现 / 拒绝，数据一律走共享上下文；
//   - 被等待的 promise 被拒绝 → 协程以该拒绝码终止（透传，与 then 的失败即停一致）；
//   - 类别（读可并发 / 写独占 / 直投不过门）：`CoStart(eKind, ...)` 给**首段**（第一个
//     await 之前那段），之后每一段由它前面那个 `CO_AWAIT` / `CO_AWAIT_ALL` 给 ——
//     协程体「挂起 → 恢复」每次都是一次独立的任务进入（挂起期间不占槽位），
//     所以类别跟段走；协程对象本身不存类别。
//
// JS / C# 对照：
//   await p;                     →  CO_AWAIT(类别, p);         // 等待一条 promise（类别 = 恢复后那一段）
//   await Promise.all([a, b]);   →  CO_AWAIT_ALL(类别, a, b);  // 并行等待多条 promise
//   return;                      →  CO_RETURN_VOID(); / CO_END();
//   return result;               →  CO_RETURN(CPromiseResult::Reject(std::runtime_error("原因")));
//   局部变量跨 await             →  必须写成派生类成员（无栈约束）
//
// 用法：
// @code
// CPromiseResult StepLoad(CPromiseResult upResult, const std::shared_ptr<CMyContext>& spCtx);
// CPromiseResult StepSave(CPromiseResult upResult, const std::shared_ptr<CMyContext>& spCtx);
//
// class CMyCoroutine : public common::async::CCoroutine<CMyContext>
// {
// public:
//     using common::async::CCoroutine<CMyContext>::CCoroutine;   // 继承上下文构造
//
//     void Run() override
//     {
//         CO_BEGIN();
//         // 每个 await 都给「恢复后那一段」的类别（这里整条协程都只读写本上下文 → 写档）
//         CO_AWAIT(common::async::TaskKind::kWrite, NewPromise(StepLoad, common::async::TaskKind::kWrite));
//         CO_AWAIT_ALL(common::async::TaskKind::kWrite, NewPromise(StepSave, common::async::TaskKind::kWrite),
//                      NewPromise(StepNotify, common::async::TaskKind::kWrite));   // 并行等待
//         CO_RETURN_VOID();                   // 正常结束（兑现）
//         CO_END();
//     }
// };
//
// // 起协程（类别必填：读可并发 / 写独占）
// std::shared_ptr<CMyCoroutine> pCoro = exec.CoStart<CMyCoroutine>(common::async::TaskKind::kWrite, spCtx);
// common::async::CPromiseResult r = pCoro->Await();   // 阻塞取最终结果
// (void)r;
// @endcode
//
// 约束（无栈协程固有）：
//  - 跨 await 的变量必须存放为派生类成员（帧），不能用函数内局部变量
//    （局部变量的声明会与 switch-case 恢复点冲突）；
//  - 协程体仍需 CO_BEGIN / CO_END 包裹（Duff's device 的 switch 骨架）；
//  - 每个协程宏独占一行（__LINE__ 作恢复点标签，同一行两个宏会冲突）；
//  - CoStart 返回的 shared_ptr 须持有到完成；框架内部 Resume / 回调捕获自持
//    强引用，提前释放也不会悬垂（对象存活到最后一个 Resume 执行完）。
// ====================================================================

namespace common {
namespace async {

namespace detail {

/// @brief 并行 await 组状态（CO_AWAIT_ALL 用）。
struct CAwaitAllGroup
{
    std::atomic<int> nPending;   ///< 剩余未完成的 promise 数。
    std::atomic<int> bRejected;  ///< 是否已有 promise 被拒绝（0/1）。

    /// 首个拒绝的「整份结果」（含文案）：写入方由 bRejected 的 CAS 独占，
    /// 读取方在 nPending 减到 0 之后（fetch_sub 的 acq_rel 保证可见）。
    CPromiseResult resultFirst;

    CAwaitAllGroup() : nPending(0), bRejected(0), resultFirst()
    {}
};

}  // namespace detail

/// @brief 无栈协程基类（基于异步 promise 的顺序化写法）。
///
/// 派生类实现协程体 Run()，用 CO_BEGIN / CO_AWAIT / CO_RETURN_VOID / CO_END
/// 宏写成「async/await」风格：await 即挂起（return 让出线程），被等待的
/// promise settled 后由执行器投递 Resume 继续。
///
/// 数据不走 await 返回值，而是共享上下文：协程与它起的子 promise 共用同一个
/// std::shared_ptr<TContext>（GetContext() 取用）。
///
/// @tparam TContext 共享上下文类型。
template <typename TContext>
class CCoroutine
{
public:
    //================ Types ================

    /// 处理器类型（与 promise 的 then 层一致：看不到上游结果）。
    using ThenHandler = detail::ThenHandler<TContext>;

    //================ Lifecycle ================

    /// @brief 创建协程（未绑定执行器；经 CAsyncExecutor::CoStart 启动）。
    ///
    /// @param spContext 共享上下文（「必传」：与 promise 一致，框架不做懒创建）。
    explicit CCoroutine(const std::shared_ptr<TContext>& spContext)
        : m_pCore(std::make_shared<detail::CPromiseCore<TContext> >(std::shared_ptr<detail::CExecutorHandle>(), spContext)),
          m_pSegment(std::make_shared<detail::CPromiseState>()),
          m_pExec(nullptr),
          m_wpSelf(),
          m_hot()
    {
        // 完成标记层只被 settle（不过门、不跑业务流程）：类别用簿记档（trace 里显示「直」）。
        m_pSegment->SetKind(detail::kKindBookkeeping);
        // 上下文强制传入：把契约钉在唯一入口上（与 promise 一致）。
        ASSERT_MSG(spContext != nullptr, "协程的共享上下文必须由调用方传入（框架不做懒创建）");
    }

    /// @brief 析构（不阻塞；虚析构：派生类成员需要正常析构）。
    virtual ~CCoroutine()
    {}

    /// @brief 不可拷贝（协程帧与运行状态唯一）。
    CCoroutine(const CCoroutine&) = delete;
    CCoroutine& operator=(const CCoroutine&) = delete;

    /// @brief 协程体（派生类实现，用 CO_BEGIN / ... / CO_END 宏）。
    virtual void Run() = 0;

    //================ Context & Await ================

    /// @brief await：阻塞获取协程最终结果（JS await 的阻塞版，不抛异常）。
    ///
    /// @warning 与 promise 的 `Await()` 同为「阻塞等待」，会占住当前工作线程；在协程体内 /
    ///          本协程执行器线程上调用它等自己 → 必死锁。预警与 promise 共用一处
    ///          （`detail::kDiagAwaitRisk`，只报告、不改变行为）。
    ///
    /// @return 最终结果：正常结束为兑现；await 到拒绝 / 执行器停止为对应拒绝码。
    CPromiseResult Await() const
    {
        // 未启动就没有执行器去跑协程 → 永远等不到结果（必挂死），属于用法错误。
        ASSERT_MSG(m_pExec != nullptr, "Await 须在 CoStart 启动之后调用（未启动的协程永远不会完成）");
        ReportBlockingRisk();
        return m_pSegment->Await();
    }

    /// @brief 共享上下文（恒非空：由调用方在建协程时传入）。
    ///
    /// 协程与它起的子 promise（NewPromise()）共用同一实例。
    std::shared_ptr<TContext> GetContext() const
    {
        return m_pCore->Context();
    }

    /// @brief 本协程作为可等待的 promise（供外层 CO_AWAIT 或 OnSettled 使用）。
    ///
    /// 须在 CAsyncExecutor::CoStart 启动之后调用（启动时绑定执行器并复位状态）。
    ///
    /// @return 指向本协程完成状态的 promise 句柄。
    CPromise<TContext> AsPromise() const
    {
        ASSERT_MSG(m_pExec != nullptr, "AsPromise 须在 CoStart 启动之后调用（启动时才绑定执行器）");
        return CPromise<TContext>::Make(m_pCore, m_pSegment);
    }

    /// @brief 起一条子 promise（复用本协程的执行器与共享上下文）。
    ///
    /// 供协程体内 await 使用：CO_AWAIT(类别, NewPromise(StepLoad, 类别))。
    /// 与 `exec.NewPromise(spCtx, handler)` 走同一条起链路径（建首层 + 强制投递首层）；
    /// 未启动（`m_pExec == nullptr`，句柄还是空）时首层投递失败 → 该 promise 以系统侧失败 `Stopped()` 收口。
    ///
    /// @param fnHandler 首层处理器（固定签名）。
    /// @param eKind 本子链的读写类别（**必填**：读可并发 / 写独占）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 指向首层的 promise 句柄（须先挂起 await，勿丢弃）。
    CPromise<TContext> NewPromise(const ThenHandler& fnHandler, TaskKind eKind, const CSourceLoc& loc = CSourceLoc()) const
    {
#if defined(ASYNC_DEBUG_TRACE)
        // trace：父层钉成「启动本协程的那一层」—— 协程体可能在别处的层栈里内联恢复，
        // 只靠「当前正在跑的层」会随调度而变。
        const detail::CChainAdopterScope scope(m_spOwnerLayer);
#endif
        // 子链用指定类别（类别逐层自负：一条链的层可以各不相同）。
        const std::shared_ptr<detail::CPromiseCore<TContext> > pCore =
            std::make_shared<detail::CPromiseCore<TContext> >(m_pCore->Handle(), m_pCore->Context());
        return CPromise<TContext>::StartChain(pCore, eKind, fnHandler, loc);
    }

protected:
    //================ Macro API ================

    /// @brief 当前恢复点（状态机步号；CO_BEGIN 的 switch 用）。
    int Step() const
    {
        return m_hot.nStep.load();
    }

    /// @brief await 一条 promise（CO_AWAIT 用）：挂起，promise settled 后恢复。
    ///
    /// 被等待的 promise 被拒绝 → 本协程以该拒绝码终止（与 then 的失败即停一致）。
    /// 数据不经返回值传递：协程与被等待的 promise 共用共享上下文。
    ///
    /// @param nLine 恢复点标签（宏自动传 __LINE__）。
    /// @param eKind **恢复后那一段**的类别（**必填**：读可并发 / 写独占 / 直投不过门）——
    ///        协程体的每一段都是一次独立的任务进入（挂起期间不占槽位），所以类别随段给：
    ///        只做只读校验的那段写 `kRead`，要改模块状态的那段写 `kWrite`。
    /// @tparam TOtherContext 被等待 promise 的上下文类型（「可与本协程不同」 —— 支持把
    ///         别的子流程（另一套 TContext）当作一个异步步骤等进来）。
    /// @param promise 被等待的 promise（含子协程 AsPromise()）。
    template <typename TOtherContext>
    void AwaitWait(int nLine, TaskKind eKind, const CPromise<TOtherContext>& promise)
    {
        ASSERT_MSG(m_pExec != nullptr, "await 只能在 CoStart 启动之后（协程体 Run() 内）调用");
        m_hot.nStep.store(nLine, std::memory_order_release);

        // 回调捕获自持强引用：保证协程对象存活到回调执行完毕。
        // 「恢复后那一段以什么身份过门」也由这个闭包带走（eKind）—— 类别不存协程身上：
        // 触发恢复的闭包就在这里登记，顺手带上比「先写成员、等别的线程来读」更直接（也不涉及可见性）。
        std::shared_ptr<void> spSelf = m_wpSelf.lock();
        promise.OnSettled(
            [spSelf, this, eKind](CPromiseResult result)
            {
                if (result.IsRejected())
                {
                    MarkTerminated(result);  // 被等待的 promise 被拒绝 → 协程终止（码透传）。
                }
                ResumeInline(eKind);  // 就地续跑 / 投递回本执行器（负载感知）。
            });
    }

    /// @brief 并行 await 多条 promise（CO_AWAIT_ALL 用）：全部 settled 后恢复。
    ///
    /// 任一条被拒绝 → 协程以首个拒绝码终止（仍等全部结束，避免对象提前释放）。
    ///
    /// @param nLine 恢复点标签（宏自动传 __LINE__）。
    /// @param eKind **全部落定后那一段**的类别（**必填**：读可并发 / 写独占 / 直投不过门）。
    /// @param args 被等待的 promise 列表（可为 NewPromise(...) 表达式、AsPromise() 句柄，
    ///             或「其它上下文类型」的子流程 promise）。
    template <typename... TArgs>
    void AwaitAll(int nLine, TaskKind eKind, TArgs&&... args)
    {
        m_hot.nStep.store(nLine, std::memory_order_release);

        std::shared_ptr<detail::CAwaitAllGroup> pGroup = std::make_shared<detail::CAwaitAllGroup>();
        pGroup->nPending.store(static_cast<int>(sizeof...(TArgs)), std::memory_order_relaxed);
        if (sizeof...(TArgs) == 0)
        {
            return;  // 空列表到不了这里（C++11 的宏展开要求至少给一条 promise），留作防御。
        }

        AwaitEach(pGroup, eKind, std::forward<TArgs>(args)...);
    }

    /// @brief 本协程是否已终止（await 到拒绝）。
    bool IsTerminated() const
    {
        return m_hot.bTerminated.load(std::memory_order_acquire);  // acquire：同下面的终止结果配对
    }

    /// @brief 终止结果（IsTerminated() 为 true 时有效）—— await 到的那份结果整份保留（码 / 文案 / 来源）。
    const CPromiseResult& TerminateResult() const
    {
        return m_hot.resultTerminate;
    }

    /// @brief 协程以指定结果结束（CO_RETURN 用）。
    ///
    /// @param result 最终结果（兑现 / 拒绝）。
    void CompleteResult(const CPromiseResult& result)
    {
        m_pSegment->Settle(result);
    }

    /// @brief 协程正常结束（兑现）：CO_RETURN_VOID / CO_END 用。
    void CompleteDone()
    {
        m_pSegment->Settle(CPromiseResult::Resolve());
    }

    /// @brief 协程以终止结果结束（await 到拒绝后的统一出口）。
    void CompleteTerminated()
    {
        m_pSegment->Settle(m_hot.resultTerminate);
    }

private:
    //================ Internal ================

    friend class CAsyncExecutor;  // Start / SetSelf（CoStart 启动路径）。

    /// @brief 阻塞等待前的「死锁预警」（与 promise 的 `ReportBlockingRisk` 同一判定，只报告）。
    ///
    /// 两种形态：① 在层内 / 协程体内阻塞（`InlineDepth() > 0`）—— 占住 worker；
    /// ② 在本协程自己的执行器线程上等本协程 —— 后续恢复需要这条线程。要避免永久挂住
    /// 请改用 `OnSettled` / 外层 `CO_AWAIT` / `AwaitFor`（promise 侧）。
    void ReportBlockingRisk() const
    {
        if (m_pSegment->IsSettled())
        {
            return;  // 已经落定：不会阻塞。
        }
        if (detail::InlineDepth() > 0 || (m_pExec != nullptr && m_pExec->IsInExecutorThread()))
        {
            ReportDiagnostic(detail::kDiagAwaitRisk);
        }
    }

    /// @brief 在指定执行器上启动协程（绑定 + 复位 + 投递首次执行）。
    ///
    /// 由 CAsyncExecutor::CoStart 调用；执行器须存活于协程生命周期
    /// （未启动 / 已停止时协程立即以系统侧失败 `Stopped()` 结束）。
    ///
    /// @param pExec 执行器指针。
    /// @param eKind 首段的类别（**必填**：读可并发 / 写独占 / 直投不过门）—— 首段 =
    ///        协程体里**第一个 await 之前**那段代码（含这次 Resume 本身）；之后每段各自声明。
    void Start(CAsyncExecutor* pExec, TaskKind eKind)
    {
        BindExecutor(pExec);
        Reset();
#if defined(ASYNC_DEBUG_TRACE)
        m_spOwnerLayer = detail::CurrentLayerState();  // trace：记下「启动协程的那一层」
#endif
        PostResume(eKind);
    }

    /// @brief 注入自持弱引用（CoStart 调用；Resume / 回调生命周期加固）。
    ///
    /// 使已投递的 Resume 与 await 回调捕获强引用：调用方提前释放 shared_ptr
    /// 后，协程对象仍存活到最后一个 Resume 执行完毕（不悬垂）。
    ///
    /// @param sp 协程对象的 shared_ptr（CoStart 返回的那个）。
    void SetSelf(const std::shared_ptr<void>& sp)
    {
        m_wpSelf = sp;
    }

    /// @brief 协程热状态：步号 / 终止标志 / 终止结果（紧邻打包，减少跨线程迁移的 cache line 数）。
    struct CHotState
    {
        std::atomic<int> nStep;         ///< 状态机步号（恢复点）。
        std::atomic<bool> bTerminated;  ///< await 到拒绝 → 终止。

        /// 终止结果（await 到的那份整份保留，含文案）：写入方在结算线程，读取方在协程线程；
        /// 用 bTerminated 的 release / acquire 发布与获取（同 nStep 的做法）。
        CPromiseResult resultTerminate;

        CHotState() : nStep(0), bTerminated(false), resultTerminate()
        {}
    };

    /// @brief 绑定执行器（Start 调用）。
    ///
    /// @param pExec 执行器指针（Resume 调度与子 promise 投递用；须存活于协程）。
    void BindExecutor(CAsyncExecutor* pExec)
    {
        m_pExec = pExec;
        if (pExec != nullptr)
        {
            m_pCore->SetHandle(pExec->Handle());  // 子 promise 与本协程共用句柄。
        }
    }

    /// @brief 复位状态（Start 调用；启动前清空上一次运行的状态）。
    void Reset()
    {
        m_pSegment = std::make_shared<detail::CPromiseState>();
        m_pSegment->SetKind(detail::kKindBookkeeping);  // 完成标记层：只被 settle，不过门。
        m_hot.nStep.store(0, std::memory_order_relaxed);
        m_hot.bTerminated.store(false, std::memory_order_relaxed);
        m_hot.resultTerminate = CPromiseResult();  // 复位成「已兑现」占位（仅 bTerminated 为真时读）。
    }

    /// @brief 把 Resume 投递到执行器（执行器不可用 → 以拒绝结束，不悬垂）。
    ///
    /// 投递的 Resume 捕获自持强引用：调用方提前释放 shared_ptr 后，协程对象
    /// 仍存活到 Resume 执行完毕。
    ///
    /// @param eKind 本次恢复那一段的类别（过门用）。
    void PostResume(TaskKind eKind)
    {
        if (m_pExec == nullptr)
        {
            Terminate(CPromiseResult::Reject(std::runtime_error("执行器已停")));
            return;
        }
        std::shared_ptr<void> spSelf = m_wpSelf.lock();
        if (!spSelf)
        {
            Terminate(CPromiseResult::Reject(std::runtime_error("执行器已停")));  // 无强引用（理论不应发生）。
            return;
        }
        if (!m_pExec->Post(eKind,
                [spSelf, this]()
                {
                    Resume();
                }))
        {
            Terminate(CPromiseResult::Reject(std::runtime_error("执行器已停")));  // 执行器已停止 / 不可用。
        }
    }

    /// @brief 内联续接（就地续跑 + 负载感知）：await 回调已运行在工作线程上，
    ///        只有当前线程就是本协程自己的执行器线程、且线程池无积压时才直接继续执行
    ///        协程体（省去一次入队 + 唤醒）；跨执行器（典型：等别的模块的 promise）或有
    ///        积压则投递 —— 保证协程体始终跑在自己的执行器线程上。
    ///
    /// 与 promise 的级联共用线程局部深度计数，限制连续内联层数防爆栈。
    ///
    /// @param eKind 本次恢复那一段的类别（过门 / 就地判定用）。
    void ResumeInline(TaskKind eKind)
    {
        if (m_pExec == nullptr || m_pExec->IsStopped())
        {
            Terminate(CPromiseResult::Reject(std::runtime_error("执行器已停")));
            return;
        }
        // 就地判定与 promise 层派发共用一处（多一条「线程池无积压」的负载感知条件）：
        // 类别按**本次恢复那一段**判（恢复闭包带过来的，见 AwaitWait / Start）：
        // 只有当前线程正持着本门同类的槽位时才就地 —— 否则恢复要排队
        //（读段里等到的协程恢复不能就地跑写代码，反之亦然）。
        if (detail::ShouldInline(m_pExec->Handle(), eKind, /* bRequireIdle = */ true))
        {
            detail::CInlineGuard guard;  // 深度 +1 / -1 成对。
            Resume();
            return;
        }

        PostResume(eKind);  // 跨执行器 / 有积压 / 深度超限：投递，回本执行器线程 / 保并行度 / 防爆栈。
    }

    /// @brief 在当前线程继续执行协程体（状态机从恢复点继续）。
    ///
    /// 终止判定由协程体宏完成（case 处 IsTerminated() → CompleteTerminated()），
    /// 此处不拦截，保证被终止的协程也能走到完成（Await() 不阻塞）。
    ///
    /// **协程体抛异常 → 本协程以该异常收口**（与 promise 的层一致：层里抛异常 = 本层拒绝）。
    /// 兜在这里是必须的：恢复路径的上游是线程池 worker（`CThreadPool::WorkerLoop` 不捕获异常），
    /// 而执行器的 guard 只会把它记成一条诊断、**不会 settle 协程** —— 协程会永久 pending，
    /// `Await()` 死等。
    void Resume()
    {
        try
        {
            Run();
        }
        catch (const std::exception& e)
        {
            // 文案带走（类型降级为 runtime_error，与层处理器的约定一致）。
            Terminate(CPromiseResult::Reject(std::runtime_error(e.what())));
        }
        catch (...)
        {
            Terminate(CPromiseResult::Reject(std::runtime_error("处理器异常")));
        }
    }

    /// @brief 标记终止（不 settle；等待协程体走到统一出口）。
    ///
    /// 结果整份留下（含码与文案）—— 与 settle 的那份是同一个值（24 字节拷贝，无分配）。
    void MarkTerminated(const CPromiseResult& result)
    {
        m_hot.resultTerminate = result;
        m_hot.bTerminated.store(true, std::memory_order_release);  // release：发布上面的写入
    }

    /// @brief 标记终止并立即 settle（同步失败的出口：协程体不会再被恢复）。
    void Terminate(const CPromiseResult& result)
    {
        MarkTerminated(result);
        m_pSegment->Settle(result);
    }

    /// @brief 并行 await：递归展开等待列表（终止重载）。
    void AwaitEach(const std::shared_ptr<detail::CAwaitAllGroup>& /*pGroup*/, TaskKind /*eKind*/)
    {}

    /// @brief 并行 await：递归展开等待列表（注册一条 promise 的 settled 通知）。
    ///
    /// @tparam TOtherContext 被等待 promise 的上下文类型（允许与协程不同）。
    /// @tparam TRest 其余被等待的 promise。
    /// @param pGroup 并行组状态。
    /// @param eKind 全部落定后那一段的类别（原样带到恢复闭包里）。
    /// @param promise 当前注册的 promise。
    /// @param rest 其余 promise。
    template <typename TOtherContext, typename... TRest>
    void AwaitEach(const std::shared_ptr<detail::CAwaitAllGroup>& pGroup, TaskKind eKind, const CPromise<TOtherContext>& promise,
        TRest&&... rest)
    {
        ASSERT_MSG(m_pExec != nullptr, "await 只能在 CoStart 启动之后（协程体 Run() 内）调用");
        std::shared_ptr<void> spSelf = m_wpSelf.lock();
        promise.OnSettled(
            [pGroup, spSelf, this, eKind](CPromiseResult result)
            {
                OnAwaitDone(pGroup, eKind, result);
            });
        AwaitEach(pGroup, eKind, std::forward<TRest>(rest)...);
    }

    /// @brief 并行 await：一条 promise settled（记首个拒绝结果；全部结束时恢复 / 终止）。
    ///
    /// @param pGroup 并行组状态。
    /// @param eKind 全部落定后那一段的类别（过门用）。
    /// @param result 本条子 promise 的结果。
    void OnAwaitDone(const std::shared_ptr<detail::CAwaitAllGroup>& pGroup, TaskKind eKind, const CPromiseResult& result)
    {
        if (result.IsRejected())
        {
            int nExpected = 0;
            if (pGroup->bRejected.compare_exchange_strong(nExpected, 1))
            {
                pGroup->resultFirst = result;  // 只有 CAS 赢家写（后续读取在 nPending 归零之后）。
            }
        }
        if (pGroup->nPending.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            if (pGroup->bRejected.load(std::memory_order_relaxed))
            {
                MarkTerminated(pGroup->resultFirst);  // 以首个拒绝的整份结果终止（含文案）。
            }
            ResumeInline(eKind);  // 就地续跑或投递回本执行器（负载感知）。
        }
    }

    std::shared_ptr<detail::CPromiseCore<TContext> > m_pCore;  ///< 共享核心（上下文 + 执行器句柄）。
    std::shared_ptr<detail::CPromiseState> m_pSegment;         ///< 协程完成状态（AsPromise 暴露）。
    CAsyncExecutor* m_pExec;                                   ///< 执行器指针（Resume 调度 + 子 promise 投递）。
    std::weak_ptr<void> m_wpSelf;                              ///< 自持弱引用（生命周期加固）。
    CHotState m_hot;                                           ///< 热状态（步号 / 终止标志 / 拒绝码）。
#if defined(ASYNC_DEBUG_TRACE)
    std::shared_ptr<detail::CPromiseState> m_spOwnerLayer;  ///< 启动协程的那一层（trace；未启动 → 空）。
#endif
};

/// @brief 起协程实现（执行器入口）：创建 + 注入自持引用 + 启动。
///
/// @tparam TCoroutine 协程类型（继承 CCoroutine<TContext> 并实现 Run()）。
/// @tparam TArgs 协程构造参数类型。
/// @param eKind **首段**的类别（**必填**：读可并发 / 写独占 / 直投不过门）—— 首段 =
///        第一个 await 之前那段（含首次 Resume）；之后每段由 `CO_AWAIT` / `CO_AWAIT_ALL` 给。
/// @param args 转发给 TCoroutine 构造函数的参数。
/// @return 协程对象；调用方须持有直到完成（Await() 取结果），勿丢弃。
template <typename TCoroutine, typename... TArgs>
std::shared_ptr<TCoroutine> CAsyncExecutor::CoStart(TaskKind eKind, TArgs&&... args)
{
    std::shared_ptr<TCoroutine> pCoro = std::make_shared<TCoroutine>(std::forward<TArgs>(args)...);
    pCoro->SetSelf(pCoro);      // 自持弱引用：Resume / 回调生命周期加固。
    pCoro->Start(this, eKind);  // 绑定 + 复位 + 投递首次执行（未启动 / 已停止 → 立即被拒绝）。
    return pCoro;
}

}  // namespace async
}  // namespace common

// ====================================================================
// 协程体宏（Duff's device 状态机；每个宏独占一行，__LINE__ 作恢复点）。
// 使用形态（派生类成员函数 Run() 内，async/await 风格）：
//
//   void Run() override
//   {
//       CO_BEGIN();
//       CO_AWAIT(TaskKind::kRead, NewPromise(StepLoad, TaskKind::kRead));       // 等待子 promise（被拒绝则终止）
//       CO_AWAIT(TaskKind::kWrite, pChild->AsPromise());                        // 等待子协程；恢复后这段要改状态
//       CO_AWAIT_ALL(TaskKind::kWrite, NewPromise(StepA, TaskKind::kRead),
//                    NewPromise(StepB, TaskKind::kWrite));                       // 并行等待多条 promise
//       CO_RETURN_VOID();                                                       // 正常结束（兑现）
//       CO_END();                                                               // 兜底：正常结束
//   }
//
// 说明：① await 不传递数据（层与层、协程与 promise 之间只传兑现 / 拒绝），
//         数据读写一律通过 GetContext() 得到的共享上下文；
//       ② 每个 await 的第一个参数是**恢复后那一段**的类别（必填，与其它异步入口一致）：
//         这段代码以读身份还是写身份过读写门，由它决定 —— 只读的段用 `kRead`（可与别的读并发），
//         改模块状态的段用 `kWrite`，纯搬运且不碰模块状态的段可以用 `kDirect`。
// ====================================================================
#define CO_BEGIN()  \
    switch (Step()) \
    {               \
        case 0:;

#define CO_AWAIT(eKind, expr)             \
    AwaitWait(__LINE__, (eKind), (expr)); \
    return;                               \
    case __LINE__:                        \
        if (IsTerminated())               \
        {                                 \
            CompleteTerminated();         \
            return;                       \
        }

#define CO_AWAIT_ALL(eKind, ...)              \
    AwaitAll(__LINE__, (eKind), __VA_ARGS__); \
    return;                                   \
    case __LINE__:                            \
        if (IsTerminated())                   \
        {                                     \
            CompleteTerminated();             \
            return;                           \
        }

#define CO_RETURN(result)     \
    CompleteResult((result)); \
    return;

#define CO_RETURN_VOID() \
    CompleteDone();      \
    return;

#define CO_END()    \
    }               \
    CompleteDone(); \
    return;
