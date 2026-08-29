// ====================================================================
// 轻量性能 / 压力测试框架（自包含，仅依赖 C++11 标准库）
//
// 设计目标：
//   - 微基准：对「一个逻辑操作」做 预热 + 自适应批处理 + 多轮采样，
//     输出 均值 / P50 / P99 / 吞吐(ops/s)。短操作自动分批以摊销
//     测量循环自身的开销，长操作（含异步调度）直接单次测。
//   - 压力测试：固定时长内重复「提交 W 个任务 → 等待全部完成」的
//     窗口，丢弃预热窗口，统计稳定窗口吞吐。
//   - 结果注册：所有用例把 Result 写入 Registry，最后由 Report.h
//     汇总成 markdown 表格。
//
// 用法示例（微基准）：
//   benchmark::BenchOp("分组", "实现名",
//       []() { /* 一个逻辑操作 */ }, /*采样数*/ 41, "说明");
//
// 用法示例（压力测试）：
//   std::atomic<uint64_t> done(0);
//   benchmark::StressWindow("分组", "实现名", 500, 2000,
//       [&]() { engine.Submit([&done](){ done.fetch_add(1); }); }, done,
//       "窗口 500 任务 / 持续 2s");
// ====================================================================
#ifndef COM_BENCHMARK_FRAMEWORK_BENCH_H
#define COM_BENCHMARK_FRAMEWORK_BENCH_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace benchmark {

using Clock = std::chrono::steady_clock;

// ====================================================================
// 单条基准结果
// ====================================================================
struct Result
{
    std::string group;      // 分组（表格 section 标题）。
    std::string name;       // 实现 / 用例名。
    double mean_ns = 0.0;   // 均值 ns/op（压力模式下为 平均窗口耗时 ns）。
    double p50_ns = 0.0;    // P50 ns。
    double p99_ns = 0.0;    // P99 ns。
    double ops_per_sec = 0.0; // 吞吐（ops/s）。
    std::string note;       // 说明（参数 / 环境）。
    bool is_stress = false; // true：压力结果（表格按压力列渲染）。
};

// ====================================================================
// 结果注册表：收集所有用例结果，最后统一输出。
// ====================================================================
class Registry
{
public:
    static Registry& Instance()
    {
        static Registry s_instance;
        return s_instance;
    }

    void Add(const Result& r) { m_results.push_back(r); }

    const std::vector<Result>& Results() const { return m_results; }

    void Clear() { m_results.clear(); }

private:
    Registry() {}
    std::vector<Result> m_results;
};

// ====================================================================
// 高精度时钟（纳秒）
// ====================================================================
inline double NowNs()
{
    return std::chrono::duration<double, std::nano>(Clock::now().time_since_epoch()).count();
}

// ====================================================================
// 微基准执行器：预热 + 自适应批处理 + 多轮采样。
//
// @param group    分组名（表格 section）。
// @param name     实现 / 用例名。
// @param fn       一个逻辑操作（同步语义：含异步时须自行等待完成）。
// @param samples  采样轮数（默认 41，P99 需要一定样本量）。
// @param note     说明文字。
// ====================================================================
inline void BenchOp(const std::string& group, const std::string& name,
                    std::function<void()> fn, int samples = 41,
                    const std::string& note = std::string())
{
    // 1) 预热（丢弃：让缓存、线程、分配器就绪）。
    for (int i = 0; i < 8; ++i)
        fn();

    // 2) 探测单次耗时 → 决定批大小（目标 100μs / 批，摊销循环开销）。
    const int kProbe = 200;
    const double kTargetNs = 100000.0; // 100μs
    double t0 = NowNs();
    for (int i = 0; i < kProbe; ++i)
        fn();
    double t1 = NowNs();
    double nsPerOp = (t1 - t0) / kProbe;
    if (nsPerOp <= 0.0)
        nsPerOp = 1.0;
    int batch = 1;
    if (nsPerOp < kTargetNs)
    {
        batch = static_cast<int>(kTargetNs / nsPerOp);
        if (batch < 1)
            batch = 1;
        if (batch > 200000)
            batch = 200000;
    }

    // 3) 多轮采样（每轮测一个批的总耗时 → 平均到单次）。
    std::vector<double> vec;
    vec.reserve(static_cast<size_t>(samples));
    for (int s = 0; s < samples; ++s)
    {
        double a = NowNs();
        for (int i = 0; i < batch; ++i)
            fn();
        double b = NowNs();
        vec.push_back((b - a) / batch);
    }

    // 4) 统计 均值 / P50 / P99。
    std::sort(vec.begin(), vec.end());
    double sum = 0.0;
    for (size_t i = 0; i < vec.size(); ++i)
        sum += vec[i];
    double mean = sum / vec.size();
    double p50 = vec[vec.size() / 2];
    size_t idx99 = static_cast<size_t>(vec.size() * 0.99);
    if (idx99 >= vec.size())
        idx99 = vec.size() - 1;
    double p99 = vec[idx99];

    Result r;
    r.group = group;
    r.name = name;
    r.mean_ns = mean;
    r.p50_ns = p50;
    r.p99_ns = p99;
    r.ops_per_sec = (mean > 0.0) ? 1e9 / mean : 0.0;
    r.note = note;
    Registry::Instance().Add(r);
}

// ====================================================================
// 等待 N 个异步任务完成（基于原子计数，忙等 + 退避）。
// ====================================================================
inline void WaitDone(const std::atomic<uint64_t>& done, uint64_t target)
{
    while (done.load(std::memory_order_acquire) < target)
        std::this_thread::yield();
}

// ====================================================================
// 窗口式压力测试：固定时长内重复「提交 window 个任务 → 等待完成」，
// 丢弃前 kWarmup 个预热窗口，统计稳定窗口平均吞吐。
//
// @param group        分组名。
// @param name         实现名。
// @param window       每个窗口的任务数。
// @param duration_ms  持续时长（毫秒）。
// @param submit       提交一个任务（任务体必须对 done 执行 fetch_add）。
// @param done         共享完成计数器（每窗口开始前框架会清零）。
// @param note         说明。
// ====================================================================
inline void StressWindow(const std::string& group, const std::string& name,
                         int window, int duration_ms,
                         const std::function<void()>& submit,
                         std::atomic<uint64_t>& done,
                         const std::string& note = std::string())
{
    const int kWarmup = 2; // 丢弃的预热窗口数。
    const double tEndNs = NowNs() + duration_ms * 1e6;

    // 预热窗口（丢弃）。
    for (int w = 0; w < kWarmup; ++w)
    {
        done.store(0, std::memory_order_relaxed);
        for (int i = 0; i < window; ++i)
            submit();
        WaitDone(done, static_cast<uint64_t>(window));
    }

    // 正式采样：记录每个窗口耗时（秒）。
    std::vector<double> winSec;
    while (NowNs() < tEndNs)
    {
        done.store(0, std::memory_order_relaxed);
        double a = NowNs();
        for (int i = 0; i < window; ++i)
            submit();
        WaitDone(done, static_cast<uint64_t>(window));
        double b = NowNs();
        winSec.push_back((b - a) / 1e9);
    }
    if (winSec.empty())
        winSec.push_back(0.000001); // 兜底，避免除零。

    double sum = 0.0;
    for (size_t i = 0; i < winSec.size(); ++i)
        sum += winSec[i];
    double avgSec = sum / winSec.size();
    double ops = (avgSec > 0.0) ? window / avgSec : 0.0;

    Result r;
    r.group = group;
    r.name = name;
    r.mean_ns = (ops > 0.0) ? 1e9 / ops : 0.0;
    r.p50_ns = 0.0;
    r.p99_ns = 0.0;
    r.ops_per_sec = ops;
    r.note = note;
    r.is_stress = true;
    Registry::Instance().Add(r);
}

} // namespace benchmark

#endif // COM_BENCHMARK_FRAMEWORK_BENCH_H
