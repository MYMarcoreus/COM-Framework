#pragma once

/// @file AsyncTestKit.h
/// 跨模块异步用例的共享脚手架（测试专用，不参与库构建）。
///
/// 只放**与业务无关**的两类东西，避免各用例重复抄同样的代码：
///  - 观测工具：步骤轨迹（含每步所在线程）+ 并发/步数探针；
///  - 被调模块：自持 1 线程执行器、固定两步（连库 B1 → 读库 B2），可配延时 / 拒绝 / Stop。
///
/// 调用方模块（订单 / 流程）**不放在这里**：每个用例的流程形状不同，留在各自文件里更自解释。
///
/// 用法：`#include "AsyncTestKit.h"` + 按需 `using asynctest::CTraceSink;` 等（测试文件内，逐个引入，
/// 不写 `using namespace asynctest;`）。

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"
#include "Async/PromiseResult.h"

namespace asynctest {

/// @brief 步骤轨迹 + 每步所在线程（跨模块共享，写入加锁）。
struct CTraceSink
{
    std::mutex mutex;                                                ///< 保护 strTrace / vecSteps。
    std::string strTrace;                                            ///< 步骤轨迹（如 "A1;B1;B2;A2;A3;"）。
    std::vector<std::pair<std::string, std::thread::id> > vecSteps;  ///< 步骤 → 所在线程。

    /// @brief 追加一步（同时记录所在线程）。
    ///
    /// @param strStep 步骤标签（如 "A1"）。
    void Append(const char* strStep)
    {
        std::lock_guard<std::mutex> lock(mutex);
        strTrace += strStep;
        strTrace += ';';
        vecSteps.push_back(std::make_pair(std::string(strStep), std::this_thread::get_id()));
    }

    /// @brief 取某一步所在线程。
    ///
    /// @param strStep 步骤标签。
    ///
    /// @return 该步所在线程（未找到时返回默认构造的 thread::id）。
    std::thread::id ThreadOf(const char* strStep) const
    {
        for (size_t i = 0; i < vecSteps.size(); ++i)
        {
            if (vecSteps[i].first == strStep)
            {
                return vecSteps[i].second;
            }
        }
        return std::thread::id();
    }

    /// @brief 某一步出现的次数。
    ///
    /// @param strStep 步骤标签。
    ///
    /// @return 出现次数。
    int Count(const char* strStep) const
    {
        int nCount = 0;
        for (size_t i = 0; i < vecSteps.size(); ++i)
        {
            if (vecSteps[i].first == strStep)
            {
                ++nCount;
            }
        }
        return nCount;
    }
};

/// @brief 并发 / 步数探针（跨链共享；全原子，测试自身不引入数据竞争）。
struct CStepProbe
{
    std::atomic<int> nOrderInFlight;     ///< 调用方模块自有步骤当前并发数。
    std::atomic<int> nOrderMaxInFlight;  ///< 调用方模块自有步骤并发峰值。
    std::atomic<int> nOrderSteps;        ///< 调用方模块自有步骤总数。
    std::atomic<int> nStockInFlight;     ///< 被调模块步骤当前并发数。
    std::atomic<int> nStockMaxInFlight;  ///< 被调模块步骤并发峰值。
    std::atomic<int> nStockSteps;        ///< 被调模块步骤总数。

    CStepProbe()
        : nOrderInFlight(0),
          nOrderMaxInFlight(0),
          nOrderSteps(0),
          nStockInFlight(0),
          nStockMaxInFlight(0),
          nStockSteps(0)
    {}

    /// @brief 更新峰值。
    ///
    /// @param nPeak 峰值计数。
    /// @param nNow 当前值。
    static void UpdatePeak(std::atomic<int>& nPeak, int nNow)
    {
        int nMax = nPeak.load();
        while (nNow > nMax && !nPeak.compare_exchange_weak(nMax, nNow))
        {
        }
    }

    /// @brief 进入调用方模块自有步骤。
    void EnterOrder()
    {
        ++nOrderSteps;
        UpdatePeak(nOrderMaxInFlight, ++nOrderInFlight);
    }

    /// @brief 离开调用方模块自有步骤。
    void LeaveOrder()
    {
        --nOrderInFlight;
    }

    /// @brief 进入被调模块步骤。
    void EnterStock()
    {
        ++nStockSteps;
        UpdatePeak(nStockMaxInFlight, ++nStockInFlight);
    }

    /// @brief 离开被调模块步骤。
    void LeaveStock()
    {
        --nStockInFlight;
    }
};

// ---- 探针的自由函数入口（未接探针时空操作） ----

/// @brief 调用方模块步骤进入。
inline void EnterOrderStep(const std::shared_ptr<CStepProbe>& pProbe)
{
    if (pProbe != nullptr)
    {
        pProbe->EnterOrder();
    }
}

/// @brief 调用方模块步骤离开。
inline void LeaveOrderStep(const std::shared_ptr<CStepProbe>& pProbe)
{
    if (pProbe != nullptr)
    {
        pProbe->LeaveOrder();
    }
}

/// @brief 被调模块步骤进入。
inline void EnterStockStep(const std::shared_ptr<CStepProbe>& pProbe)
{
    if (pProbe != nullptr)
    {
        pProbe->EnterStock();
    }
}

/// @brief 被调模块步骤离开。
inline void LeaveStockStep(const std::shared_ptr<CStepProbe>& pProbe)
{
    if (pProbe != nullptr)
    {
        pProbe->LeaveStock();
    }
}

/// @brief 模拟耗时（拉长窗口，便于暴露并发重叠）。
inline void SleepMs(int nMs)
{
    if (nMs > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(nMs));
    }
}

/// @brief 被调模块上下文（两步：连库 → 读库）。
struct CCalleeCtx
{
    int nSku;                             ///< 入参：商品号。
    int nAvail;                           ///< 出参：可用库存。
    int nDelayMs;                         ///< 每步模拟耗时。
    bool bReject;                         ///< 第二步是否拒绝。
    int nRejectCode;                      ///< 拒绝码（默认 kBusinessBase）。
    std::shared_ptr<CTraceSink> spTrace;  ///< 轨迹（跨模块共享观测点，可空）。
    std::shared_ptr<CStepProbe> pProbe;   ///< 探针（可空）。
    std::thread::id idConnect;            ///< 第一步所在线程。
    std::thread::id idRead;               ///< 第二步所在线程。

    CCalleeCtx() : nSku(0), nAvail(5), nDelayMs(0), bReject(false), nRejectCode(common::async::kBusinessBase)
    {}
};

/// @brief 被调模块：自持 1 线程执行器；两步都在自己的执行器上跑；可显式 Stop。
class CCalleeModule
{
public:
    CCalleeModule() : m_exec(1)
    {
        m_exec.Start();
    }

    /// @brief 停止本模块执行器（之后的投递一律被拒绝）。
    void Stop()
    {
        m_exec.Stop();
    }

    /// @brief 查询库存：两步（调用方拿不到本模块执行器）。
    ///
    /// @param spCtx 本模块上下文。
    ///
    /// @return 本层链的 promise。
    common::async::CPromise<CCalleeCtx> QueryStockAsync(const std::shared_ptr<CCalleeCtx>& spCtx)
    {
        return m_exec.NewPromise(spCtx, &StepConnect, ASYNC_LOC).Then(&StepRead, ASYNC_LOC);
    }

private:
    /// 第一步：模拟连库。
    static common::async::CPromiseResult StepConnect(
        common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CCalleeCtx>& spCtx)
    {
        EnterStockStep(spCtx->pProbe);
        spCtx->idConnect = std::this_thread::get_id();
        if (spCtx->spTrace != nullptr)
        {
            spCtx->spTrace->Append("B1");
        }
        SleepMs(spCtx->nDelayMs);
        LeaveStockStep(spCtx->pProbe);
        return common::async::CPromiseResult::Resolve();
    }

    /// 第二步：读库存（与第一步串行、同线程）；`bReject` 时以 `nRejectCode` 拒绝。
    static common::async::CPromiseResult StepRead(
        common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CCalleeCtx>& spCtx)
    {
        EnterStockStep(spCtx->pProbe);
        spCtx->idRead = std::this_thread::get_id();
        if (spCtx->spTrace != nullptr)
        {
            spCtx->spTrace->Append("B2");
        }
        SleepMs(spCtx->nDelayMs);
        LeaveStockStep(spCtx->pProbe);
        if (spCtx->bReject)
        {
            return common::async::CPromiseResult::Reject(spCtx->nRejectCode);
        }
        spCtx->nAvail = 5;
        return common::async::CPromiseResult::Resolve();
    }

    common::async::CAsyncExecutor m_exec;  ///< 模块私有执行器（单线程）。
};

}  // namespace asynctest
