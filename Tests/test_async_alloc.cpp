/// @file test_async_alloc.cpp
/// 异步链的分配计数护栏：把「每层堆分配次数」钉成回归测试，防止无意的额外分配。
///
/// 为什么需要它：promise 链的开销几乎全在堆分配上（每次分配 ~15 ns，一次无竞争
/// 加锁仅 ~2 ns），优化收益也只有靠分配次数才能稳定度量。计数方式：覆盖全局
/// `operator new` / `operator delete`，仅在测量窗口内累加，窗口外只多一次
/// relaxed 读。窗口外不计数的原因见 `CAllocCounter`。
///
/// 当前预算（steady state，`Common/Async/Promise.h`；数字为实测值）：
///  - 建链（挂层）：**2 次/层** —— `make_shared<CPromiseState>`（层状态）+ 处理器
///    `std::function`；每链另有不超过 8 次的常数（核心、首层 runner）；
///  - 跑链（任务体投递）：**1 次/层** —— `MakeHandlerRunner` 造的任务体（`Post` 路径）；
///  - 合计 **3 次/层**；断言只设上限，因此后续把每层做到 2 次（把任务体塞进层状态）也会通过。
///
/// 怎么把「建链」与「跑链」分开量：**在层函数内部测量** —— 单线程执行器此刻正被本层占用，
/// 新建的链不会立刻开跑，于是窗口里只有建链分配（框架已不再提供「延迟启动」，
/// 不需要它也能拿到确定的测量窗口）。
///
/// 覆盖：
///  - 挂层：绝对上限 + 「不随层数增长」的差值检查（消掉每链常数）；
///  - 任务体投递：`Post` 路径的每次分配（链的每一层都要投一个）；
///  - 两者合起来 = 一条 N 层链的 3 次/层（建链 2 + 跑链 1）。

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <thread>

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
    long long nValue;                      ///< 层函数累加值，用于确认层真的执行过。
    common::async::CAsyncExecutor* pExec;  ///< 测量用执行器（层内起链，见文件头）。
    std::shared_ptr<CAllocCtx> spOther;    ///< 测量用上下文（预先造好，避免污染窗口）。
    int nMeasureLayers;                    ///< 本次测量层数。
    long long nMeasuredCounts;             ///< 输出：测量到的分配次数。
    long long nMeasuredBytes;              ///< 输出：测量到的分配字节数。

    CAllocCtx() : nValue(0), pExec(nullptr), spOther(), nMeasureLayers(0), nMeasuredCounts(0), nMeasuredBytes(0)
    {}
};

/// 层：上下文计数 +1。
static common::async::CPromiseResult StepBump(common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CAllocCtx>& spCtx)
{
    ++spCtx->nValue;
    return common::async::CPromiseResult::Resolve();
}

/// 层：在层内测量「建一条 nMeasureLayers 层的链」的堆分配。
///
/// 此刻执行器线程正被本层占用（单线程执行器）→ 新建链的首层不会立即执行，
/// 窗口里只有建链分配，结果确定。
static common::async::CPromiseResult StepMeasureBuild(
    common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CAllocCtx>& spCtx)
{
    CAllocCounter counter;
    common::async::CPromise<CAllocCtx> tail = spCtx->pExec->NewPromise(spCtx->spOther, &StepBump, ASYNC_LOC);
    for (int i = 0; i < spCtx->nMeasureLayers; ++i)
    {
        tail = tail.Then(&StepBump, ASYNC_LOC);
    }
    counter.Stop();

    spCtx->nMeasuredCounts = counter.Counts();
    spCtx->nMeasuredBytes = counter.Bytes();
    return common::async::CPromiseResult::Resolve();
}

// ==================== 建链分配预算 ====================

/// @brief 建链每层堆分配 ≤ 2 次，且不随层数增长。
TEST(AsyncAlloc_BuildBudget)
{
    const int kSmallLayers = 100;  ///< 小链层数。
    const int kBigLayers = 500;    ///< 大链层数。
    const int kSlack = 8;          ///< 每链常数分配的余量。

    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());
    std::shared_ptr<CAllocCtx> spCtx = std::make_shared<CAllocCtx>();
    spCtx->pExec = &exec;
    spCtx->spOther = std::make_shared<CAllocCtx>();

    // 每次测量都在「被测量的层内」进行（单线程执行器被本层占着 → 新建链不会立即开跑）。
    spCtx->nMeasureLayers = kSmallLayers;
    ASSERT_TRUE(exec.NewPromise(spCtx, &StepMeasureBuild, ASYNC_LOC).Await().IsFulfilled());
    const long long nSmallCounts = spCtx->nMeasuredCounts;
    const long long nSmallBytes = spCtx->nMeasuredBytes;

    spCtx->nMeasureLayers = kBigLayers;
    ASSERT_TRUE(exec.NewPromise(spCtx, &StepMeasureBuild, ASYNC_LOC).Await().IsFulfilled());
    const long long nBigCounts = spCtx->nMeasuredCounts;
    const long long nBigBytes = spCtx->nMeasuredBytes;

    std::printf("      %d 层：%lld 次 / %lld 字节（每层 %.2f 次、%.0f 字节）\n", kSmallLayers, nSmallCounts, nSmallBytes,
        static_cast<double>(nSmallCounts) / kSmallLayers, static_cast<double>(nSmallBytes) / kSmallLayers);
    std::printf("      %d 层：%lld 次 / %lld 字节（每层 %.2f 次、%.0f 字节）\n", kBigLayers, nBigCounts, nBigBytes,
        static_cast<double>(nBigCounts) / kBigLayers, static_cast<double>(nBigBytes) / kBigLayers);

    ASSERT_TRUE(nSmallCounts <= 2 * kSmallLayers + kSlack);
    ASSERT_TRUE(nBigCounts <= 2 * kBigLayers + kSlack);

    // 差值检查：多出的层只应带来 2 次/层的成本（消掉每链常数，避免常数掩盖线性增长）。
    ASSERT_TRUE(nBigCounts - nSmallCounts <= 2 * (kBigLayers - kSmallLayers) + kSlack);

    // 测量层本身只计数、不改 nValue（被测量的链用的是 spOther）。
    ASSERT_EQ(spCtx->nValue, 0);
    exec.Stop();
}

// ==================== 跑链分配预算 ====================

/// @brief 跑链每层堆分配 ≤ 1 次 —— 每个层任务体（`MakeHandlerRunner`）。
///
/// 把「建链」与「跑链」分开的诀窍：先用一个**占位任务把唯一的 worker 占住**，
/// 于是窗口外建好的链只登记、不执行（首层在队列里等着）。窗口内放行并等待，
/// 整条链就在这次等待里跑完 —— 窗口里只有跑链分配，与调度时序无关。
TEST(AsyncAlloc_RunBudget)
{
    const int kLayers = 200;  ///< 层数（> kMaxInlineDepth，覆盖「内联级联」与「改投递」两段路径）。
    const int kSlack = 8;     ///< 常数余量。

    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    // 占位任务：占住唯一 worker，直到本测试放行。
    std::atomic<bool> bOccupied(false);
    std::atomic<bool> bRelease(false);
    ASSERT_TRUE(exec.Post(
        [&bOccupied, &bRelease]()
        {
            bOccupied.store(true);
            while (!bRelease.load())
            {
                std::this_thread::yield();
            }
        }));
    while (!bOccupied.load())
    {
        std::this_thread::yield();
    }

    // 窗口外：建一条 N 层链（worker 被占住 → 首层只入队，不执行）。
    std::shared_ptr<CAllocCtx> spCtx = std::make_shared<CAllocCtx>();
    common::async::CPromise<CAllocCtx> chain = exec.NewPromise(spCtx, &StepBump, ASYNC_LOC);
    for (int i = 0; i < kLayers; ++i)
    {
        chain = chain.Then(&StepBump, ASYNC_LOC);
    }

    // 窗口内：放行 worker 并等链跑完 → 统计到的就是「跑链」的分配。
    CAllocCounter counter;
    bRelease.store(true);
    const common::async::CPromiseResult result = chain.AwaitFor(5000);
    counter.Stop();

    std::printf("      %d 层：跑链 %lld 次（每层 %.2f 次）\n", kLayers, counter.Counts(),
        static_cast<double>(counter.Counts()) / kLayers);

    ASSERT_TRUE(result.IsFulfilled());
    ASSERT_EQ(spCtx->nValue, static_cast<long long>(kLayers) + 1);
    ASSERT_TRUE(counter.Counts() <= kLayers + kSlack);
    exec.Stop();
}
