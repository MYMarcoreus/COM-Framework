/// @file test_async_alloc.cpp
/// 异步链的分配计数护栏：把「每层堆分配次数」钉成回归测试，防止无意的额外分配。
///
/// 为什么需要它：promise 链的开销几乎全在堆分配上（每次分配 ~15 ns，一次无竞争
/// 加锁仅 ~2 ns），优化收益也只有靠分配次数才能稳定度量。计数方式：覆盖全局
/// `operator new` / `operator delete`，仅在测量窗口内累加，窗口外只多一次
/// relaxed 读。窗口外不计数的原因见 `CAllocCounter`。
///
/// 当前预算（steady state，`Common/Async/Promise.h`；数字为实测值）：
///  - 建链：**2 次/层** —— `make_shared<CPromiseState>`（层状态）+ 处理器
///    `std::function`；每链另有不超过 8 次的常数（核心、延迟起链载荷、首层 runner）；
///  - 跑链：**1 次/层** —— 投递给执行器的任务体（`MakeHandlerRunner`）；
///  - 断言只设上限，因此后续把每层做到 1 次（把任务体塞进层状态）也会通过。
///
/// 覆盖：
///  - 建链：绝对上限 + 「不随层数增长」的差值检查（消掉每链常数）；
///  - 跑链：含内联级联（前 kMaxInlineDepth 层）与改投递（其余层）两段路径。

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "Async/SourceLoc.h"
#include "TestFramework.h"

// ==================== 分配计数基础设施 ====================

namespace {

/// @brief 是否处于「计分配」窗口。
///
/// 只有测量窗口内为真：窗口外每个 new 只多一次 relaxed 读，对其它测试影响可忽略。
std::atomic<bool> g_bAllocRecord(false);

/// @brief 测量窗口内累计的堆分配次数。
std::atomic<long long> g_nAllocCount(0);

/// @brief 测量窗口内累计的堆分配字节数。
std::atomic<long long> g_nAllocBytes(0);

/// @brief 计数 + `malloc`（各变体共用的实现；不分配内存即不产生额外分配）。
///
/// @param nSize 请求字节数。
/// @return 分配到的内存；失败返回 nullptr。
void* MallocCounted(size_t nSize)
{
    if (g_bAllocRecord.load(std::memory_order_relaxed))
    {
        g_nAllocCount.fetch_add(1, std::memory_order_relaxed);
        g_nAllocBytes.fetch_add(static_cast<long long>(nSize), std::memory_order_relaxed);
    }
    return std::malloc(nSize == 0 ? 1 : nSize);
}

}  // namespace

/// @brief 计次版的全局 `operator new`。
void* operator new(size_t nSize)
{
    void* p = MallocCounted(nSize);
    if (p == nullptr)
    {
        throw std::bad_alloc();
    }
    return p;
}

/// @brief 计次版的全局 `operator new[]`。
void* operator new[](size_t nSize)
{
    void* p = MallocCounted(nSize);
    if (p == nullptr)
    {
        throw std::bad_alloc();
    }
    return p;
}

/// @brief 计次版的全局 `operator new`（nothrow，失败返回空指针而不抛）。
void* operator new(size_t nSize, const std::nothrow_t& /*tag*/) noexcept
{
    return MallocCounted(nSize);
}

/// @brief 计次版的全局 `operator new[]`（nothrow，失败返回空指针而不抛）。
void* operator new[](size_t nSize, const std::nothrow_t& /*tag*/) noexcept
{
    return MallocCounted(nSize);
}

/// @brief 对应 `operator new` 的释放。
void operator delete(void* p) noexcept
{
    std::free(p);
}

/// @brief 对应 `operator new` 的 sized 释放。
void operator delete(void* p, size_t /*nSize*/) noexcept
{
    std::free(p);
}

/// @brief 对应 `operator new[]` 的释放。
void operator delete[](void* p) noexcept
{
    std::free(p);
}

/// @brief 对应 `operator new[]` 的 sized 释放。
void operator delete[](void* p, size_t /*nSize*/) noexcept
{
    std::free(p);
}

/// @brief 对应 nothrow `operator new` 的释放。
void operator delete(void* p, const std::nothrow_t& /*tag*/) noexcept
{
    std::free(p);
}

/// @brief 对应 nothrow `operator new[]` 的释放。
void operator delete[](void* p, const std::nothrow_t& /*tag*/) noexcept
{
    std::free(p);
}

/// @brief 堆分配计数窗口（RAII）：进入窗口开始累加，`Stop()` 结算。
///
/// 计数只在本对象存活期间生效，因此测试进程里其它用例不受影响。窗口内禁止
/// 打印 / 构造容器等会自行分配的操作，否则会把无关分配算进去。
class CAllocCounter
{
public:
    /// @brief 进入计数窗口。
    CAllocCounter()
        : m_nCount0(g_nAllocCount.load(std::memory_order_relaxed)),
          m_nBytes0(g_nAllocBytes.load(std::memory_order_relaxed)),
          m_nCounts(0),
          m_nBytes(0),
          m_bStopped(false)
    {
        g_bAllocRecord.store(true, std::memory_order_relaxed);
    }

    /// @brief 离开计数窗口（重复调用无副作用）。
    ~CAllocCounter()
    {
        Stop();
    }

    /// @brief 结算：窗口内累计的次数与字节。
    void Stop()
    {
        if (m_bStopped)
        {
            return;
        }
        g_bAllocRecord.store(false, std::memory_order_relaxed);
        m_nCounts = g_nAllocCount.load(std::memory_order_relaxed) - m_nCount0;
        m_nBytes = g_nAllocBytes.load(std::memory_order_relaxed) - m_nBytes0;
        m_bStopped = true;
    }

    /// @brief 窗口内堆分配次数（`Stop()` 之后才有意义）。
    long long Counts() const
    {
        return m_nCounts;
    }

    /// @brief 窗口内堆分配字节数（`Stop()` 之后才有意义）。
    long long Bytes() const
    {
        return m_nBytes;
    }

private:
    CAllocCounter(const CAllocCounter&);             ///< 禁止拷贝。
    CAllocCounter& operator=(const CAllocCounter&);  ///< 禁止赋值。

    long long m_nCount0;  ///< 进入窗口时的次数基准。
    long long m_nBytes0;  ///< 进入窗口时的字节基准。
    long long m_nCounts;  ///< 窗口内次数。
    long long m_nBytes;   ///< 窗口内字节数。
    bool m_bStopped;      ///< 是否已结算（保证 Stop 幂等）。
};

// ==================== 测试用的上下文与层函数 ====================

/// @brief 分配计数测试的共享上下文（本身不分配内存）。
struct CAllocCtx
{
    long long nValue;  ///< 层函数累加值，用于确认层真的执行过。

    CAllocCtx() : nValue(0)
    {}
};

/// 层：上下文计数 +1。
static common::async::CPromiseResult StepBump(
    common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CAllocCtx>& spCtx)
{
    ++spCtx->nValue;
    return common::async::CPromiseResult::Resolve();
}

// ==================== 建链分配预算 ====================

/// @brief 建一条 `nLayers` 层的延迟起链（`BuildPromise` + `Then`），并统计该阶段分配。
///
/// 用延迟起链是刻意的：起链前不会有任何层执行，测量窗口内只有建链分配，结果确定；
/// 若用 `NewPromise`，首层会立即投递到执行器，worker 的执行分配会掺进窗口里。
///
/// @param exec 执行器（已启动）。
/// @param spCtx 共享上下文。
/// @param nLayers 层数。
/// @param nCountsOut 输出：该阶段的堆分配次数。
/// @param nBytesOut 输出：该阶段的堆分配字节数。
/// @return 链尾 promise（未起链）。
static common::async::CPromise<CAllocCtx> BuildThenChain(common::async::CAsyncExecutor& exec,
    const std::shared_ptr<CAllocCtx>& spCtx, int nLayers, long long& nCountsOut, long long& nBytesOut)
{
    CAllocCounter counter;
    common::async::CPromise<CAllocCtx> tail = exec.BuildPromise<CAllocCtx>(spCtx);
    for (int i = 0; i < nLayers; ++i)
    {
        tail = tail.Then(&StepBump, ASYNC_LOC);
    }
    counter.Stop();

    nCountsOut = counter.Counts();
    nBytesOut = counter.Bytes();
    return tail;
}

/// @brief 建链每层堆分配 ≤ 2 次，且不随层数增长。
TEST(AsyncAlloc_BuildBudget)
{
    const int kSmallLayers = 100;  ///< 小链层数。
    const int kBigLayers = 500;    ///< 大链层数。
    const int kSlack = 8;          ///< 每链常数分配的余量。

    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());
    std::shared_ptr<CAllocCtx> spCtx = std::make_shared<CAllocCtx>();

    long long nSmallCounts = 0;
    long long nSmallBytes = 0;
    long long nBigCounts = 0;
    long long nBigBytes = 0;
    common::async::CPromise<CAllocCtx> smallChain =
        BuildThenChain(exec, spCtx, kSmallLayers, nSmallCounts, nSmallBytes);
    common::async::CPromise<CAllocCtx> bigChain = BuildThenChain(exec, spCtx, kBigLayers, nBigCounts, nBigBytes);

    std::printf("      %d 层：%lld 次 / %lld 字节（每层 %.2f 次、%.0f 字节）\n", kSmallLayers, nSmallCounts,
        nSmallBytes, static_cast<double>(nSmallCounts) / kSmallLayers, static_cast<double>(nSmallBytes) / kSmallLayers);
    std::printf("      %d 层：%lld 次 / %lld 字节（每层 %.2f 次、%.0f 字节）\n", kBigLayers, nBigCounts, nBigBytes,
        static_cast<double>(nBigCounts) / kBigLayers, static_cast<double>(nBigBytes) / kBigLayers);

    ASSERT_TRUE(nSmallCounts <= 2 * kSmallLayers + kSlack);
    ASSERT_TRUE(nBigCounts <= 2 * kBigLayers + kSlack);

    // 差值检查：多出的层只应带来 2 次/层的成本（消掉每链常数，避免常数掩盖线性增长）。
    ASSERT_TRUE(nBigCounts - nSmallCounts <= 2 * (kBigLayers - kSmallLayers) + kSlack);

    // 链还没起链，层函数一次都不该跑。
    ASSERT_EQ(spCtx->nValue, 0);
    exec.Stop();
}

// ==================== 跑链分配预算 ====================

/// @brief 跑链每层堆分配 ≤ 1 次（投递给执行器的任务体）。
TEST(AsyncAlloc_RunBudget)
{
    const int kLayers = 200;  ///< 层数（> kMaxInlineDepth，覆盖「内联级联」与「改投递」两段路径）。
    const int kSlack = 8;  ///< 每链常数分配的余量。

    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());
    std::shared_ptr<CAllocCtx> spCtx = std::make_shared<CAllocCtx>();

    long long nBuildCounts = 0;
    long long nBuildBytes = 0;
    common::async::CPromise<CAllocCtx> chain = BuildThenChain(exec, spCtx, kLayers, nBuildCounts, nBuildBytes);

    CAllocCounter counter;
    chain.Start();
    const common::async::CPromiseResult result = chain.Await();
    counter.Stop();

    std::printf("      %d 层：建链 %lld 次，跑链 %lld 次 / %lld 字节（每层 %.2f 次）\n", kLayers, nBuildCounts,
        counter.Counts(), counter.Bytes(), static_cast<double>(counter.Counts()) / kLayers);

    ASSERT_TRUE(result.IsFulfilled());
    ASSERT_EQ(spCtx->nValue, static_cast<long long>(kLayers));
    ASSERT_TRUE(counter.Counts() <= kLayers + kSlack);
    exec.Stop();
}
