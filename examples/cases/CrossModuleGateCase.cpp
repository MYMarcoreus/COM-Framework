// ====================================================================
// 例子：跨模块起链时，被调模块的状态由哪扇门保护（两种写法对照）
//
// 写法①（**反例**）：公开异步函数在「起链回调」里碰自己的状态。
//   D 之前：起链回调在调用方的线程上同步跑、继承调用方的门 —— 对 B 而言是门外，
//   与 B 自己的写任务并发 → 数据竞争（TSan 报告见提交说明）。
//   本用例用「重叠进入计数器」把这件事变成可自校验的断言。
// 写法②（**正例**）：状态访问放在「本模块门内的首层」（handler 变体），
//   起链回调只做「发起 + 登记回调」。
//
// 现在两种写法都安全：起链回调与层体走同一套「就地 / 过门」派发
// （门外 / 别的门 / 换类别 → 按声明类别过门投递）。
//   —— 写法①能成立，正是 D 的价值：不需要纪律，直觉写法也是安全的。
//
// 本用例同时跑两条流：
//   · B 自己的写链（OwnWriteAsync）：门内首层碰状态；
//   · A 的跨模块链（RunFlow，**只用 then 系列**）：ThenPromise 的工厂里调 B 的公开异步函数。
// 两条流都碰 B 的同一份状态 → 若保护正确，重叠次数恒为 0、计数精确。
// ====================================================================
#include "cases/CrossModuleGateCase.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include "Assert.h"
#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"

namespace {

using common::async::CAsyncExecutor;
using common::async::CPromise;
using common::async::CPromiseResult;
using common::async::TaskKind;

/// @brief 共享上下文（每条流一个，per-call，不共享）。
struct CGateCtx
{
    CGateCtx() : nUserId(0)
    {}

    int nUserId;  ///< 目标用户 id（决定落在状态表的哪一格）。
};

/// @brief 被调模块 B：自己的执行器（自己的门），状态是普通成员（安全来自读写门）。
class CCalleeModule
{
public:
    CCalleeModule() : m_spExec(new CAsyncExecutor(2)), m_nActive(0), m_nOverlaps(0), m_nSettled(0)
    {}

    bool Start()
    {
        return m_spExec->Start();
    }

    void Stop()
    {
        m_spExec->Stop();
    }

    /// @brief 公开异步函数（写法①的形态：起链回调里碰状态）。
    ///
    /// 起链回调按**声明类别**（kWrite）过门后才跑 —— 即使调用方在别的模块的门里，
    /// 这段状态访问也落在**本模块自己的门**里（与 OwnWriteAsync 的层体互斥）。
    ///
    /// @param spCtx 本流程上下文（写入结果）。
    /// @return 本流程的 promise 句柄。
    CPromise<CGateCtx> QueryAsync(const std::shared_ptr<CGateCtx>& spCtx)
    {
        return m_spExec->NewPromise(
            spCtx,
            [this, spCtx](const CPromise<CGateCtx>::ResolveFn& fnResolve, const CPromise<CGateCtx>::RejectFn& /*fnReject*/)
            {
                EnterCritical();  // 状态段：观测是否有重叠进入
                m_mapStats[spCtx->nUserId] += 1;
                LeaveCritical();
                m_nSettled.fetch_add(1);
                fnResolve();
            },
            TaskKind::kWrite);
    }

    /// @brief 本模块自己的写链（门内首层碰状态）：驱动用例用，只用 then 系列。
    ///
    /// @param spCtx 本流程上下文。
    /// @return 本流程的 promise 句柄。
    CPromise<CGateCtx> OwnWriteAsync(const std::shared_ptr<CGateCtx>& spCtx)
    {
        return m_spExec->NewPromise(
            spCtx,
            [this](const std::shared_ptr<CGateCtx>& spSelf)
            {
                EnterCritical();
                m_mapStats[spSelf->nUserId] += 1;
                LeaveCritical();
                m_nSettled.fetch_add(1);
                return CPromiseResult::Resolve();
            },
            TaskKind::kWrite);
    }

    /// @brief 状态表取值（仅在两条流全部落定后读：安全）。
    ///
    /// @param nUserId 用户 id。
    /// @return 该格的计数。
    int StatOf(int nUserId)
    {
        return m_mapStats[nUserId];
    }

    /// @brief 状态段的重叠进入次数（应为 0）。
    ///
    /// @return 重叠次数。
    int Overlaps() const
    {
        return m_nOverlaps.load();
    }

    /// @brief 已落定的流数（校验「都在跑」）。
    ///
    /// @return 落定流数。
    int Settled() const
    {
        return m_nSettled.load();
    }

private:
    /// @brief 进入状态段：已有别的进入者 = 记录一次重叠。
    ///
    /// 进入后停留一小段：把状态段拉宽到「重叠可观测」的量级 —— 否则纳秒级的 map 自增
    /// 即使真的并发也几乎撞不上（那就是竞态最阴险的样子：平时看不出来）。
    void EnterCritical()
    {
        if (m_nActive.fetch_add(1) != 0)
        {
            m_nOverlaps.fetch_add(1);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    /// @brief 离开状态段。
    void LeaveCritical()
    {
        m_nActive.fetch_sub(1);
    }

    std::shared_ptr<CAsyncExecutor> m_spExec;  ///< 模块私有调度域（= 一扇门）
    std::map<int, int> m_mapStats;             ///< 模块状态：普通成员，安全来自读写门
    std::atomic<int> m_nActive;                ///< 当前在状态段里的进入者数（观测用）
    std::atomic<int> m_nOverlaps;              ///< 重叠进入次数（观测用，应为 0）
    std::atomic<int> m_nSettled;               ///< 已落定的流数（观测用）
};

/// @brief 调用方模块 A：自己的执行器（自己的门），链**只用 then 系列**组装。
class CCallerModule
{
public:
    explicit CCallerModule(const std::shared_ptr<CCalleeModule>& spCallee) : m_spExec(new CAsyncExecutor(2)), m_spCallee(spCallee)
    {}

    bool Start()
    {
        return m_spExec->Start();
    }

    void Stop()
    {
        m_spExec->Stop();
    }

    /// @brief A 的业务链：门内首层 → 跨模块调 B（`ThenPromise`，本层在 A 的写槽位里）→ 回到 A。
    ///
    /// 跨模块起链就发生在 `ThenPromise` 的工厂里：B 的起链回调被投递进 **B 自己的门**，
    /// 所以它可以安全地碰 B 的状态（写法①也成立）。
    ///
    /// @param spCtx 本流程上下文（透传给 B）。
    /// @return 本流程的 promise 句柄。
    CPromise<CGateCtx> RunFlow(const std::shared_ptr<CGateCtx>& spCtx)
    {
        return m_spExec
            ->NewPromise(
                spCtx,
                [](const std::shared_ptr<CGateCtx>& /*spSelf*/)
                {
                    return CPromiseResult::Resolve();
                },
                TaskKind::kWrite)
            .ThenPromise(
                [this](const std::shared_ptr<CGateCtx>& spSelf) -> CPromise<CGateCtx>
                {
                    return m_spCallee->QueryAsync(spSelf);  // 跨模块起链（本层在 A 的写槽位里）
                },
                TaskKind::kWrite)
            .Then(
                [](const std::shared_ptr<CGateCtx>& /*spSelf*/)
                {
                    return CPromiseResult::Resolve();
                },
                TaskKind::kWrite);
    }

private:
    std::shared_ptr<CAsyncExecutor> m_spExec;   ///< 模块私有调度域（= 一扇门）
    std::shared_ptr<CCalleeModule> m_spCallee;  ///< 被调模块（接口引用，不传执行器）
};

}  // namespace

bool RunCrossModuleGateCase()
{
    const int kFlows = 60;

    std::shared_ptr<CCalleeModule> spCallee(new CCalleeModule());
    CCallerModule caller(spCallee);
    if (!spCallee->Start() || !caller.Start())
    {
        std::printf("  起执行器失败\n");
        return false;
    }

    std::vector<CPromise<CGateCtx> > vecFlows;
    for (int i = 0; i < kFlows; ++i)
    {
        // B 自己的写链（门内首层碰状态）
        std::shared_ptr<CGateCtx> spOwn(new CGateCtx());
        spOwn->nUserId = 7;
        vecFlows.push_back(spCallee->OwnWriteAsync(spOwn));

        // A 的跨模块链（只用 then 系列 → ThenPromise 里调 B 的公开异步函数）
        std::shared_ptr<CGateCtx> spFlow(new CGateCtx());
        spFlow->nUserId = 3;
        vecFlows.push_back(caller.RunFlow(spFlow));
    }

    // 主线程逐条等落定（主线程不占任何槽位；这里只为「跑完再断言」）。
    for (std::size_t i = 0; i < vecFlows.size(); ++i)
    {
        vecFlows[i].Await();
    }

    caller.Stop();  // Stop = 先关门、再等已接受的跑完 → 之后读状态才是安全的
    spCallee->Stop();

    const int nStatA = spCallee->StatOf(3);
    const int nStatB = spCallee->StatOf(7);
    const int nOverlaps = spCallee->Overlaps();
    const int nSettled = spCallee->Settled();

    std::printf(
        "  跨模块链 %d 条 + 被调模块自己的写链 %d 条：状态 user3=%d user7=%d、"
        "落定 %d 条、状态段重叠进入 %d 次\n",
        kFlows, kFlows, nStatA, nStatB, nSettled, nOverlaps);

    ASSERT(nStatA == kFlows);        // 跨模块链每条加 1（不丢更新）
    ASSERT(nStatB == kFlows);        // 自己模块的写链每条加 1
    ASSERT(nSettled == 2 * kFlows);  // 两条流都真的跑完（不是被门挡死）
    ASSERT(nOverlaps == 0);          // 被调模块的状态段任意时刻只有一个进入者
    return true;
}
