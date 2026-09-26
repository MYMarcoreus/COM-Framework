#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Async/AsyncExecutor.h"
#include "Async/PromiseResult.h"
#include "Async/PromiseTypes.h"
#include "Async/SourceLoc.h"
#include "Async/Trace.h"

// ====================================================================
// 「一层」的状态机（`detail::CPromiseState`）与 settled 通知路径
//
// 从 Promise.h 拆出（2026-09-26）：这一块是**非模板**的纯状态，与「句柄 / 层运行器」无关 ——
//   - 状态是单向开关：pending → settled（fulfilled | rejected），首次 `Settle` 生效；
//   - 处理器（层 / 通知）在锁外触发（链的逐层推进就在这条调用路径上级联完成）；
//   - 通知不是层：没有结果可落、也不该占门槽位，只保证送达（执行器不可用时就地送达）。
//
// 读这一块时只要记住三件事：① 锁内只做「发布结果 + 摘走处理器」，用户代码一律在锁外跑；
// ② `m_bSettled` 是结果发布点（等待者在锁内复查它，自旋读只是快路径）；③ 公开接口顺着读：
// 构造 → `Settle` → 登记（`AddHandler` / `AddNotice` / `AddNoticeOn`，通知的包装在里面）
// → 查询与等待（`Await` / `AwaitFor` / `IsSettled` / `Kind`）→ 调试构建的 trace 设置器。
// ====================================================================

namespace common {
namespace async {

namespace detail {

/// @brief 诊断文案（集中一处：测试断言常量，而不是去匹配子串）。
constexpr const char* kDiagNoticeThrow = "OnSettled 通知里抛出了异常（已忽略；通知不是层，没有结果可落）";

// 通知的「登记」是 `CPromiseState` 的两个公开入口（`AddNotice` 就地版 / `AddNoticeOn` 指定执行器版）：
// 「恒送达 + 异常不外抛」的包装（私有 `RunNotice` / `RunNoticeOn`）收在这两个入口里 ——
// 调用点（`CPromise::OnSettled` / `OnSettledOn`）只传用户的 `SettledNotice`，不必自己包一层。

/// @brief promise 状态（对应 JS 中「每个 then 返回的新 promise」的状态）。
///
/// 一道 promise 链由若干状态串成，一个状态对应一层。状态是单向开关：
/// pending → settled（fulfilled | rejected），首次 Settle 生效；之前登记的
/// 处理器（handler）随后在锁外触发（链的逐层推进就在这条调用路径上级联完成）。
class CPromiseState
{
public:
    /// 处理器：接收上一层结果。
    using Handler = std::function<void(const CPromiseResult&)>;

    /// @brief 创建状态（pending）。
    CPromiseState() : m_eKind(TaskKind::kWrite), m_bSettled(false), m_result()
    {}

    /// @brief settle 本状态并触发处理器（锁外调用处理器，防重入死锁）。
    ///
    /// 仅首次生效；先唤醒等待者，再按注册顺序在锁外调用所有处理器。
    /// 处理器在调用方（结算）线程上被触发；若它是「层处理器」，再由 `Dispatch` 派发：
    /// 已在本链执行器线程 → 就地执行；否则投递回本链执行器。
    ///
    /// @param result 本层最终结果（已兑现 / 已拒绝）。
    void Settle(const CPromiseResult& result)
    {
        Handler handlerInline;
        std::vector<Handler> vecHandlers;

        // ① 锁内「发布结果 + 摘走处理器」：单向开关，只有第一次 settle 生效。
        //    处理器一律在锁外调用（用户代码不得在锁内跑：会重入死锁）。
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_bSettled.load(std::memory_order_relaxed))
            {
                return;  // 已 settle 过：幂等丢弃（重复 settle / settle 后再抛异常都安全）。
            }

            m_result = result;
            m_bSettled.store(true, std::memory_order_relaxed);  // 结果发布点：等待者在锁内复查它。
            handlerInline = std::move(m_handlerInline);         // 第一个处理器（1:1 链的常态）。
            vecHandlers.swap(m_vecHandlers);                    // 分叉出来的其余（按登记顺序）。
        }

        // ② 先唤醒所有等待者（同一层可被多个线程 Await）：它们只读结果，不看处理器。
        m_cv.notify_all();

        // ③ 再在锁外按登记顺序跑处理器 —— 链的逐层推进就在这条路径上级联完成。
        if (handlerInline)
        {
            handlerInline(result);
        }
        for (size_t i = 0; i < vecHandlers.size(); ++i)
        {
            if (vecHandlers[i])
            {
                vecHandlers[i](result);
            }
        }
    }

    /// @brief 登记「层处理器」（then / catch / finally / thenPromise）：已 settled 时**按本层类别过读写门**投递。
    ///
    /// 「停了的执行器不再跑新层」：执行器不可用时返回 `false`，由调用方以「执行器已停」收口本层。
    ///
    /// @param pHandle 执行器句柄（已 settled 时投递用）。
    /// @param eKind 本层类别（读 / 写 / 直投）——决定这次投递怎么过门。
    /// @param fnHandler 处理器（按值接收，登记时移动存储避免拷贝）。
    /// @return true 已登记 / 已投递；false 仅当本层已 settled 且执行器不可用。
    bool AddHandler(const std::shared_ptr<CExecutorHandle>& pHandle, TaskKind eKind, Handler fnHandler)
    {
        return Register(pHandle, &eKind, std::move(fnHandler), /* bGuaranteedDelivery = */ false);
    }

    /// @brief 登记「通知」（`OnSettled`）：**不过读写门**、执行器不可用时就地送达。
    ///
    /// 通知不是层、没有结果可落，也不该占门槽位 —— 所以这里**没有类别参数**（它不进门的队列）；
    /// 也正因如此，通知里只应做轻量搬运 / 收尾，别长时间占着模块（那会把排队中的任务一起拖住）。
    ///
    /// 「恒送达 + 异常不外抛」两条契约由本方法**统一包装**（调用点不必自己兜异常）：
    /// 传进来的就是用户的 `SettledNotice`，包装与投递策略都收在这里。
    ///
    /// @param pHandle 执行器句柄（已 settled 时投递用）。
    /// @param fnSettled 收尾通知（入参为本层最终结果）。
    /// @return 恒 true（通知绝不丢）。
    bool AddNotice(const std::shared_ptr<CExecutorHandle>& pHandle, const SettledNotice& fnSettled)
    {
        return Register(
            pHandle, nullptr,
            [fnSettled](const CPromiseResult& result)
            {
                RunNotice(fnSettled, result);
            },
            /* bGuaranteedDelivery = */ true);
    }

    /// @brief 登记「通知（指定执行器版）」（`OnSettledOn`）：送达后**在 pTarget 的线程上跑**。
    ///
    /// 与 `AddNotice` 的唯一差别：通知的**执行线程**被钉在 pTarget 上（已在该线程 → 就地，
    /// 否则投递过去）；投递不进去时仍就地送达（与 `AddNotice` 同一条「保证送达」）。
    ///
    /// @param pTarget 目标执行器句柄（通知要在它的线程上跑）。
    /// @param fnSettled 收尾通知（入参为本层最终结果）。
    /// @return 恒 true（通知绝不丢）。
    bool AddNoticeOn(const std::shared_ptr<CExecutorHandle>& pTarget, const SettledNotice& fnSettled)
    {
        return Register(
            pTarget, nullptr,
            [pTarget, fnSettled](const CPromiseResult& result)
            {
                // 与 AddNotice 对称：各一行，差异只在「去哪条线程」。
                RunNoticeOn(pTarget, fnSettled, result);
            },
            /* bGuaranteedDelivery = */ true);
    }

    /// @brief 阻塞等待本状态 settle（先短自旋，超时再阻塞等待）。
    ///
    /// @return 本层最终结果（已兑现 / 已拒绝）。
    CPromiseResult Await()
    {
        // ① 先短自旋（50 µs）：链尾的层通常已经落定，省掉一次锁 + 条件变量。
        //    relaxed 读只作宽松提示，真正的判定在下面锁内 —— 避免漏唤醒 / 读到半成品结果。
        const auto spinDeadline = std::chrono::steady_clock::now() + std::chrono::microseconds(50);
        while (!m_bSettled.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < spinDeadline)
        {
            std::this_thread::yield();
        }

        // ② 还没落定 → 在条件变量上等，直到 Settle 唤醒（m_bSettled 是结果发布点）。
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock,
            [this]()
            {
                return m_bSettled.load(std::memory_order_relaxed);
            });
        return m_result;
    }

    /// @brief 阻塞等待本状态 settle，最多等 nTimeoutMs 毫秒。
    ///
    /// @param nTimeoutMs 超时毫秒数（< 0 = 无限等待，等价 `Await()`）。
    /// @return 本层最终结果；超时返回框架侧拒绝「等待超时」（**本层未落定**）。
    CPromiseResult AwaitFor(int nTimeoutMs)
    {
        // ① 两个快路径：< 0 = 无限等待（等价 Await）；已落定 = 直接取结果（不建等待）。
        if (nTimeoutMs < 0)
        {
            return Await();
        }
        if (m_bSettled.load(std::memory_order_relaxed))
        {
            return Await();
        }

        // ② 未落定：只等 nTimeoutMs 毫秒。超时「不落定本层」，只是向调用方报「没等到」（链继续在后台跑）。
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_cv.wait_for(lock, std::chrono::milliseconds(nTimeoutMs),
                [this]()
                {
                    return m_bSettled.load(std::memory_order_relaxed);
                }))
        {
            return CPromiseResult::Reject(std::runtime_error("等待超时"));  // 超时：不落定本层，只向调用方报「没等到」。
        }
        return m_result;
    }

    /// @brief 本状态是否已 settled（兑现或拒绝）。
    bool IsSettled() const
    {
        return m_bSettled.load(std::memory_order_relaxed);
    }

    /// @brief 本层的读写类别（建层时定下、之后只读）。
    ///
    /// 调度用它决定本层的准入与就地：读层可并发进入模块，写层独占，直投层不过门
    /// （见 `Async/ReadWriteGate.h`）。同一个链里的层可以读 / 写 / 直投混排 —— 每层各自生效。
    ///
    /// @return 本层类别。
    TaskKind Kind() const
    {
        return m_eKind;
    }

    /// @brief 设置本层类别（建层状态时调一次，之后只读）。
    ///
    /// @param eKind 本层类别（读可并发 / 写独占 / 直投不过门）。
    void SetKind(TaskKind eKind)
    {
        m_eKind = eKind;
#if defined(ASYNC_DEBUG_TRACE)
        m_trace.eKind = eKind;  // trace：排障时看得到「这层是读还是写」。
#endif
    }

#if defined(ASYNC_DEBUG_TRACE)

    //================ 调用链 trace（「只在调试构建存在」） ================
    //
    // 发布构建下这段整段不参与编译（连同上面的 m_trace 成员）：trace 不是「空操作版本」，
    // 而是根本没有 —— 调用方要写 trace 相关代码，请自己用 #if defined(ASYNC_DEBUG_TRACE) 包住。

    /// @brief 设置本层的注册点源码位置。
    ///
    /// @param loc 源码位置（建议传 ASYNC_LOC）。
    void SetLoc(const CSourceLoc& loc)
    {
        m_trace.loc = loc;
    }

    /// @brief 记下本层在调用链里的位置：上游层 + 模式 + 它在哪条链上。
    ///
    /// 上游用「强引用」：层的状态是靠「上游的处理器闭包」保活的，闭包用完即毁 —— 用弱引用的话，
    /// 中间层跑完就被释放，链会被截断，而那正是排障最需要它的时候。因此这些链接
    /// 「一次写入、之后只读」（注册时设一次，永不释放），代价就是「只要下游还活着，
    /// 上游就不会被释放」—— 调试构建下整条链随尾层句柄存活。
    ///
    /// @param pUpstream 上游层（本层被挂到它上面；空 = 首层 / 层外起的链根）。
    /// @param eMode 本层模式（then / catch / finally）。
    /// @param bChainRoot 本层是不是「它那条链」的链根（起链的两处为 true，追加层为 false）。
    /// @param nChainId 本层的链号（链根：新分配的号；追加层：传上游的链号）。
    void SetTraceLink(const std::shared_ptr<CPromiseState>& pUpstream, HandlerMode eMode, bool bChainRoot, unsigned nChainId)
    {
        m_trace.upstream = pUpstream;
        m_trace.eMode = eMode;
        m_trace.bChainRoot = bChainRoot;
        m_trace.bSubChain = bChainRoot && (pUpstream != nullptr);  // 链根且有父层 = 子链
        m_trace.nChainId = nChainId;
    }

    /// @brief 分配层号（创建层状态时调一次）。
    ///
    /// @param nLayerId 层号（`detail::NextLayerId()`）。
    void SetLayerId(unsigned nLayerId)
    {
        m_trace.nLayerId = nLayerId;
    }

    /// @brief 记下「本层跑在哪条线程上」（开跑时由帧写一次，之后只读）。
    ///
    /// @param tid 当前线程 id。
    void SetRunningThread(const std::thread::id& tid)
    {
        m_trace.tid = tid;
    }

    /// @brief 记下「本层跑在哪个执行器上」（开跑时由帧写一次，之后只读）。
    ///
    /// 链跨执行器就跨链（子链有自己的执行器），所以这是逐层属性；名字串在执行器构造时分配一次，
    /// 各层共享（层记录持强引用：执行器析构后已起的链还会跑完）。
    ///
    /// @param spExecName 执行器名（空 = 未命名）。
    void SetTraceExec(const std::shared_ptr<const std::string>& spExecName)
    {
        m_trace.spExecName = spExecName;
    }

    /// @brief 记下本层处理器的耗时（落定前写一次）。
    ///
    /// @param nMs 毫秒数。
    void SetSelfDurationMs(long long nMs)
    {
        m_trace.nSelfMs = nMs;
    }

    /// @brief 本层链号（追加层用它继承上游的链号）。
    ///
    /// @return 链号。
    unsigned ChainId() const
    {
        return m_trace.nChainId;
    }

    /// @brief 本层的 trace 记录（「拷贝」）。
    ///
    /// 遍历（`VisitLayerChain` / `CurrentLayer`）拿到它之后会在上面填「视图字段」
    /// （深度 / 当前层 / 年龄 / 结果），不会影响层状态里存的那份。
    ///
    /// @return 本层记录。
    CLayerInfo LayerInfo() const
    {
        return m_trace;
    }

    /// @brief 取本层落定结果（链上能直接看到上游是兑现还是拒绝）。
    ///
    /// 带锁读：与 `Settle` 的写入同步（trace 会从「别的线程」看已经落定的上游层）。
    ///
    /// @param out 落定结果（返回 true 时有效）。
    /// @return 已落定 → true。
    bool TryGetResult(CPromiseResult& out) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_bSettled.load(std::memory_order_relaxed))
        {
            return false;
        }
        out = m_result;
        return true;
    }

#endif  // defined(ASYNC_DEBUG_TRACE)

private:
    /// @brief 执行 settled 通知（异常兜底：通知里抛异常只报告，不向外抛）。
    ///
    /// 通知不是「层」：它没有结果可落，也没人在等它。所以异常只能吞掉 ——
    /// 但绝不能放任它逃出（`Settle` 在锁外直接调用处理器，线程池 worker 不捕获异常 →
    /// 一旦逃出就是 std::terminate，整个进程完蛋）。
    ///
    /// 只服务 `AddNotice` / `AddNoticeOn`：这两条契约由那两个登记入口统一包装，
    /// 所以不需要（也不应该）对外暴露。
    ///
    /// @param fnSettled 通知处理器（可为空）。
    /// @param result 本层最终结果。
    static void RunNotice(const SettledNotice& fnSettled, const CPromiseResult& result)
    {
        if (!fnSettled)
        {
            return;
        }
        try
        {
            fnSettled(result);
        }
        catch (...)
        {
            ReportDiagnostic(kDiagNoticeThrow);
        }
    }

    /// @brief 在指定执行器上执行 settled 通知（`AddNoticeOn` 用）——与 `RunNotice` 对称。
    ///
    /// 已在该执行器线程 → 就地；否则投递过去；执行器不可用 → 就地送达
    /// （与「保证送达」一致，绝不丢通知）；异常兜底交给 `RunNotice`。
    ///
    /// @param pTarget 目标执行器句柄。
    /// @param fnSettled 通知处理器（可为空）。
    /// @param result 本层最终结果。
    static void RunNoticeOn(
        const std::shared_ptr<CExecutorHandle>& pTarget, const SettledNotice& fnSettled, const CPromiseResult& result)
    {
        if (!fnSettled)
        {
            return;
        }

        std::function<void()> fnRun = [fnSettled, result]()
        {
            RunNotice(fnSettled, result);
        };
        if (IsInExecutorThread(pTarget) || !PostToHandle(pTarget, std::move(fnRun)))
        {
            RunNotice(fnSettled, result);  // 已在目标线程 / 目标执行器不可用 → 就地（送达保证）。
        }
    }

    /// @brief 处理器登记的唯一实现（两条路径只差「按不按类别过门」与「投不出去怎么办」）。
    ///
    /// 未落定 → 只登记（settle 时在结算线程上触发）；已落定 → 立刻投递：
    ///  - `ptKind != nullptr`（层处理器）：`PostToHandle(handle, 类别, fn)` —— **过读写门**，投不出去返回 false；
    ///  - `ptKind == nullptr`（通知）：`PostToHandle(handle, fn)` —— **直投**（不过门），投不出去就地送达。
    ///
    /// @param pHandle 执行器句柄。
    /// @param ptKind 层类别（**通知传 nullptr**：通知不过门，没有类别可给）。
    /// @param fnHandler 处理器（按值接收，登记时移动存储避免拷贝）。
    /// @param bGuaranteedDelivery 投递失败时是否就地送达（通知 true；层 false）。
    /// @return 见 `AddHandler` / `AddNotice`。
    bool Register(
        const std::shared_ptr<CExecutorHandle>& pHandle, const TaskKind* ptKind, Handler fnHandler, bool bGuaranteedDelivery)
    {
        bool bFireNow = false;
        CPromiseResult result;

        // ① 锁内分两条路：本层还没 settle → 只登记（settle 时触发）；已 settle → 带着结果出去跑。
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_bSettled.load(std::memory_order_relaxed))
            {
                // 第一个处理器就地存（1:1 链的常态，免一次 vector 分配）；分叉的才进 vector。
                if (!m_handlerInline)
                {
                    m_handlerInline = std::move(fnHandler);
                }
                else
                {
                    m_vecHandlers.push_back(std::move(fnHandler));
                }

                return true;  // pending：已登记，settle 时在结算线程上触发。
            }

            bFireNow = true;
            result = m_result;
        }

        if (!bFireNow || !fnHandler)
        {
            return true;  // 已 settled 但没有处理器可跑（空 handler）：无事可做。
        }

        // ② 已 settled：优先投递到执行器异步跑（与 JS 一致，调用方不阻塞）。
        //    注意先把 handler 与结果按值拷进任务体 —— 投递失败时还要就地跑它。
        std::function<void()> fnRun = [fnHandler, result]()
        {
            fnHandler(result);
        };
        // 层：按类别过读写门（可能排一小会儿队，但不会丢）；通知：直投（不过门，保证送达）。
        const bool bPosted = (ptKind != nullptr) ? PostToHandle(pHandle, *ptKind, fnRun) : PostToHandle(pHandle, fnRun);
        if (bPosted)
        {
            return true;
        }

        // ③ 执行器不可用：两种策略分道扬镖 —— 层处理器报 false（由调用方以「执行器已停」收口本层，
        //    “停了的执行器不再跑新层”）；通知则在调用线程就地送达（绝不丢，否则桥接层永久 pending）。
        if (!bGuaranteedDelivery)
        {
            return false;
        }

        {
            CInlineGuard guard;  // 就地送达也要计内联深度（与其它内联路径共用，防极端嵌套）。
            fnRun();
        }
        return true;
    }

    mutable std::mutex m_mutex;          ///< 保护结果与处理器列表（mutable：trace 的只读取结果要加锁）。
    std::condition_variable m_cv;        ///< 通知等待者。
    Handler m_handlerInline;             ///< 第一个处理器（1:1 链常态，免 vector 分配）。
    std::vector<Handler> m_vecHandlers;  ///< 第二个起（同层分叉）才用。
    TaskKind m_eKind;                    ///< 本层读写类别（读 / 写 / 直投）。
    std::atomic<bool> m_bSettled;        ///< 是否已 settled（自旋读用）。
    CPromiseResult m_result;             ///< 最终结果（settled 后有效）。
#if defined(ASYNC_DEBUG_TRACE)
    /// 本层的 trace 记录（注册点 / 上游 / 模式 / 层号 / 链号 / 线程 / 耗时）。
    /// 类型就是 `CLayerInfo`（既是存储记录也是遍历视图，只此一份，没有第二个结构）。
    CLayerInfo m_trace;
#endif
};

}  // namespace detail
}  // namespace async
}  // namespace common
