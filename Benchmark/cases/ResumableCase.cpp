#include "ResumableCase.h"

#include "Async/AsyncExecutor.h"
#include "Async/Coroutine.h"
#include "framework/Bench.h"

#include <atomic>
#include <memory>
#include <string>

namespace {

/// 固定 10 次 await 的切换协程（对齐 librf yield_switch：协程内多次挂起/恢复）。
/// 完成时对共享计数 +1。m_nAwait 为构造签名冗余（保持可扩展），固定展开 10 次。
class ResumableCoro : public common::async::CCoroutine<int>
{
public:
    explicit ResumableCoro(int /*nAwait*/, std::atomic<uint64_t>* pDone)
        : m_pDone(pDone), m_v(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_INTO(m_v, []() { return 1; });
        CO_AWAIT_INTO(m_v, [this]() { return m_v + 1; });
        CO_AWAIT_INTO(m_v, [this]() { return m_v + 1; });
        CO_AWAIT_INTO(m_v, [this]() { return m_v + 1; });
        CO_AWAIT_INTO(m_v, [this]() { return m_v + 1; });
        CO_AWAIT_INTO(m_v, [this]() { return m_v + 1; });
        CO_AWAIT_INTO(m_v, [this]() { return m_v + 1; });
        CO_AWAIT_INTO(m_v, [this]() { return m_v + 1; });
        CO_AWAIT_INTO(m_v, [this]() { return m_v + 1; });
        CO_AWAIT_INTO(m_v, [this]() { return m_v + 1; });
        if (m_pDone != nullptr)
            m_pDone->fetch_add(1, std::memory_order_release);
        CO_RETURN(m_v);
        CO_END();
    }

private:
    std::atomic<uint64_t>* m_pDone;
    int m_v;
};

} // namespace

void RunResumableCases()
{
    const std::string group = "6. 协程创建 / 切换（对齐 librf resumable_switch）";
    const int kThreads = 4;
    const int kAwaitPerCoro = 10; // 每协程固定 10 次 await。
    const int grad[] = { 100, 1000, 10000 };

    for (size_t gi = 0; gi < 3; ++gi)
    {
        const int N = grad[gi];
        const uint64_t totalSwitch = static_cast<uint64_t>(N) * kAwaitPerCoro;
        common::async::CAsyncExecutor exec(static_cast<size_t>(kThreads));
        exec.Start();
        std::atomic<uint64_t> done(0);

        // ---- 创建阶段：CoStart N 个协程（投递首 Resume）----
        double t0 = benchmark::NowNs();
        for (int i = 0; i < N; ++i)
        {
            std::shared_ptr<ResumableCoro> p =
                exec.CoStart<ResumableCoro>(kAwaitPerCoro, &done);
            (void)p; // 生命周期由框架自持强引用保证，提前释放安全。
        }
        double t1 = benchmark::NowNs();
        uint64_t d1 = done.load(std::memory_order_acquire);
        double createNs = (t1 - t0) / N; // 创建 + 投递（含少量并行执行）。

        // ---- 切换阶段：等剩余协程完成，统计「新完成」的切换数 ----
        benchmark::WaitDone(done, static_cast<uint64_t>(N));
        double t2 = benchmark::NowNs();
        uint64_t switched = (static_cast<uint64_t>(N) - d1) * kAwaitPerCoro;
        if (switched == 0)
            switched = 1; // 兜底，避免除零。
        double switchNs = (t2 - t1) / switched; // 每次 CO_AWAIT 挂起+恢复。

        exec.Stop();

        // 创建成本入库。
        {
            benchmark::Result r;
            r.group = group;
            r.name = "CCoroutine create x" + std::to_string(N);
            r.mean_ns = createNs;
            r.p50_ns = createNs;
            r.p99_ns = createNs;
            r.ops_per_sec = createNs > 0 ? 1e9 / createNs : 0.0;
            r.note = "CoStart 投递（make_shared + 首 Resume），4 线程；"
                     "含后台并行执行，小数量（x100）并行不足数值偏高";
            benchmark::Registry::Instance().Add(r);
        }
        // 切换成本入库。
        {
            benchmark::Result r;
            r.group = group;
            r.name = "CCoroutine switch x" + std::to_string(totalSwitch);
            r.mean_ns = switchNs;
            r.p50_ns = switchNs;
            r.p99_ns = switchNs;
            r.ops_per_sec = switchNs > 0 ? 1e9 / switchNs : 0.0;
            r.note = "每次 CO_AWAIT 挂起+恢复（10 次/协程 × " +
                     std::to_string(N) + " 协程），4 线程";
            benchmark::Registry::Instance().Add(r);
        }
    }
}
