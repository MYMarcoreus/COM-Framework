/// @file test_common.cpp
/// Common 基础库单元测试：CBuffer / CThreadPool / CTimerManager / CConfig / CAsyncExecutor。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "TestFramework.h"

#include "Async/AsyncExecutor.h"
#include "Config/Config.h"
#include "Network/Buffer.h"
#include "Thread/ThreadPool.h"
#include "Timer/TimerManager.h"

/// @brief 缓冲区追加/读取/清空。
TEST(Buffer_AppendReadRetrieve)
{
    common::network::CBuffer buffer;
    ASSERT_EQ(buffer.Readable(), static_cast<size_t>(0));

    const char* strData = "hello";
    buffer.Append(strData, 5);
    ASSERT_EQ(buffer.Readable(), static_cast<size_t>(5));
    ASSERT_EQ(std::memcmp(buffer.Peek(), strData, 5), 0);

    buffer.RetrieveAll();
    ASSERT_EQ(buffer.Readable(), static_cast<size_t>(0));

    buffer.Append(std::string("world"));
    ASSERT_EQ(buffer.Readable(), static_cast<size_t>(5));
}

/// @brief 线程池提交任务并执行完毕。
TEST(ThreadPool_SubmitTasks)
{
    common::thread::CThreadPool pool(2);
    ASSERT_TRUE(pool.Start());

    std::atomic<int> nCounter(0);
    for (int i = 0; i < 20; ++i)
    {
        ASSERT_TRUE(pool.Submit([&nCounter]() { nCounter.fetch_add(1); }));
    }
    pool.Stop(); // 等待所有已提交任务执行完毕
    ASSERT_EQ(nCounter.load(), 20);
}

/// @brief 线程池突发并行：单线程突发投递长任务时应唤醒多个工作线程并行执行。
///
/// 回归：Submit 仅在「队列空→非空」时 notify_one，突发批量任务会退化为
/// 单线程顺序执行（峰值并发≈1）；修复后按空闲线程数补唤醒，8 线程下应显著并行。
TEST(ThreadPool_BurstParallelism)
{
    common::thread::CThreadPool pool(8);
    ASSERT_TRUE(pool.Start());

    std::atomic<int> nActive(0);
    std::atomic<int> nPeak(0);
    std::atomic<long> nDone(0);
    const int kTasks = 64;

    // 单线程突发投递（不等待），验证工作线程并行度。
    for (int i = 0; i < kTasks; ++i)
    {
        ASSERT_TRUE(pool.Submit([&nActive, &nPeak, &nDone]()
        {
            const int nNow = nActive.fetch_add(1) + 1;
            int nCur = nPeak.load();
            while (nCur < nNow && !nPeak.compare_exchange_weak(nCur, nNow))
            {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            nActive.fetch_sub(1);
            nDone.fetch_add(1);
        }));
    }
    pool.Stop(); // 等待全部完成
    ASSERT_EQ(nDone.load(), kTasks);
    ASSERT_TRUE(nPeak.load() >= 4); // 8 线程下突发应显著并行（修复前可能退化为 1）
}

/// @brief 定时器一次性触发。
TEST(Timer_OneShotFires)
{
    common::timer::CTimerManager timerManager;
    ASSERT_TRUE(timerManager.Start());

    std::atomic<int> nFired(0);
    common::timer::TimerId nId = timerManager.AddTimer(30, [&nFired]() { nFired.fetch_add(1); });
    ASSERT_TRUE(nId != common::timer::kInvalidTimerId);

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    ASSERT_EQ(nFired.load(), 1);
}

/// @brief 配置读取字符串与整数。
TEST(Config_LoadAndGet)
{
    const char* strPath = "/tmp/test_config_common.ini";
    std::ofstream ofs(strPath);
    ofs << "name = ServerA\n" << "port = 9500\n";
    ofs.close();

    common::config::CConfig config;
    ASSERT_TRUE(config.LoadFile(strPath));
    ASSERT_EQ(config.GetString("name", ""), std::string("ServerA"));
    ASSERT_EQ(config.GetString("missing", "def"), std::string("def"));
    ASSERT_EQ(config.GetInt("port", 0), 9500);
    ASSERT_EQ(config.GetInt("missing", -1), -1);
}

/// @brief 链式异步任务结果。
TEST(Async_SubmitAndGet)
{
    common::async::CAsyncExecutor executor(1);
    ASSERT_TRUE(executor.Start());

    common::async::CTaskResult<int> r = executor.Submit([]() { return 42; }).Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 42);
    executor.Stop();
}

// ==================== 无异常版异步框架（common::async，Option 风格） ====================

/// @brief 链式任务成功（Submit → Then → Then → Get，有值传播）。
TEST(NothrowAsync_ChainGet)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    common::async::CTaskResult<int> r =
        exec.Submit([]() { return 3; })
            .Then([](int n) { return n * 2; })
            .Then([](int n) { return n + 1; })
            .Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 7);
    ASSERT_TRUE(static_cast<bool>(r)); // operator bool
    exec.Stop();
}

/// @brief 无值终止沿链传播（上游任务无值，Then 不执行）。
TEST(NothrowAsync_NonePropagate)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    common::async::CTaskResult<int> r =
        exec.Submit([]() -> int { throw std::runtime_error("boom"); }) // 上游无值
            .Then([](int n) { return n + 1; })                          // 此步被跳过
            .Get();
    ASSERT_TRUE(!r.HasValue());
    ASSERT_EQ(r.Reason(), common::async::detail::kException); // 终止原因透传
    exec.Stop();
}

/// @brief 任务函数抛出异常 → 转为无值终止（原因 kException）。
TEST(NothrowAsync_ExceptionToNone)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    common::async::CTaskResult<int> r =
        exec.Submit([]() -> int { throw std::runtime_error("boom"); }).Get();
    ASSERT_TRUE(!r.HasValue());
    ASSERT_EQ(r.Reason(), common::async::detail::kException);
    exec.Stop();
}

/// @brief 未启动执行器直接 Submit → 无值终止（原因 kNotStarted）。
TEST(NothrowAsync_NotStarted)
{
    common::async::CAsyncExecutor exec(1);
    common::async::CTaskResult<int> r = exec.Submit([]() { return 1; }).Get();
    ASSERT_TRUE(!r.HasValue());
    ASSERT_EQ(r.Reason(), common::async::detail::kNotStarted);
}

/// @brief 链式 flatMap：变换函数返回 CTask，自动平铺为下游结果。
TEST(NothrowAsync_FlatMap)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    common::async::CTaskResult<int> r =
        exec.Submit([]() { return 3; })
            .Then([&exec](int n) {
                return exec.Submit([n]() { return n * 10; });
            })
            .Then([](int n) { return n + 5; })
            .Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 35);
    exec.Stop();
}

/// @brief flatMap 内部任务无值终止 → 终止沿链传播。
TEST(NothrowAsync_FlatMapNonePropagate)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    common::async::CTaskResult<int> r =
        exec.Submit([]() { return 1; })
            .Then([&exec](int) {
                return exec.Submit([]() -> int { throw std::runtime_error("inner"); }); // 内部任务无值
            })
            .Get();
    ASSERT_TRUE(!r.HasValue());
    ASSERT_EQ(r.Reason(), common::async::detail::kException);
    exec.Stop();
}

/// @brief 变换函数直接返回 CTaskResult：有值传播、无值（None）终止。
TEST(NothrowAsync_ThenReturnsResult)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    // 变换返回 CTaskResult：有值继续接龙
    common::async::CTaskResult<int> r =
        exec.Submit([]() { return 3; })
            .Then([](int n) -> common::async::CTaskResult<int> {
                if (n < 0)
                {
                    return common::async::None; // 无值 → 终止
                }
                return n * 10; // 有值（隐式）
            })
            .Then([](int n) { return n + 5; })
            .Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 35);

    // 变换返回 None → 中途终止，后续 Then 不执行
    common::async::CTaskResult<int> r2 =
        exec.Submit([]() { return -3; })
            .Then([](int n) -> common::async::CTaskResult<int> {
                if (n < 0)
                {
                    return common::async::None;
                }
                return n * 10;
            })
            .Then([](int n) { return n + 5; })
            .Get();
    ASSERT_TRUE(!r2.HasValue());
    ASSERT_EQ(r2.Reason(), common::async::detail::kEndNone);
    exec.Stop();
}

/// @brief 无返回值任务（CTaskResult&lt;void&gt;：完成即 HasValue）。
TEST(NothrowAsync_VoidTask)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::atomic<int> nDone(0);
    common::async::CTaskResult<void> r =
        exec.Submit([&nDone]() { nDone.fetch_add(1); }).Get();
    ASSERT_TRUE(r.HasValue()); // void 完成
    ASSERT_EQ(nDone.load(), 1);
    exec.Stop();
}

/// @brief void 任务支持 Then（无参数往下传），可继续链。
TEST(NothrowAsync_VoidThenChain)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    common::async::CTaskResult<int> r =
        exec.Submit([]() { /* void 任务 */ })
            .Then([]() { return 42; }) // void → int（无参数传入）
            .Then([](int n) { return n + 1; })
            .Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 43);
    exec.Stop();
}

/// @brief void→void 变换：void 任务 Then 返回 void 变换，链继续（正常完成）。
TEST(NothrowAsync_VoidToVoid)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::atomic<int> nSteps(0);
    common::async::CTaskResult<void> r =
        exec.Submit([&nSteps]() { nSteps.fetch_add(1); })      // void
            .Then([&nSteps]() { nSteps.fetch_add(1); })        // void → void
            .Get();
    ASSERT_TRUE(r.HasValue()); // void→void 链正常完成
    ASSERT_EQ(nSteps.load(), 2);
    exec.Stop();
}

/// @brief 非 void 上游变换返回 void：消费但不产出，下游为 void 完成。
TEST(NothrowAsync_ValueToVoid)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::atomic<int> nConsumed(0);
    common::async::CTaskResult<void> r =
        exec.Submit([]() { return 7; })                               // int
            .Then([&nConsumed](int n) { nConsumed.fetch_add(n); })   // int → void
            .Get();
    ASSERT_TRUE(r.HasValue()); // 非 void → void 链正常完成
    ASSERT_EQ(nConsumed.load(), 7);
    exec.Stop();
}

/// @brief 并发多任务 + 多线程 Get。
TEST(NothrowAsync_ConcurrentGet)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    std::vector<common::async::CTask<int> > tasks;
    for (int i = 0; i < 8; ++i)
    {
        tasks.push_back(exec.Submit([i]() { return i * i; }));
    }
    std::atomic<int> nOk(0);
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
    {
        threads.push_back(std::thread([&tasks, i, &nOk]() {
            common::async::CTaskResult<int> r = tasks[i].Get();
            if (r.HasValue() && r.Value() == i * i)
            {
                nOk.fetch_add(1);
            }
        }));
    }
    for (size_t i = 0; i < threads.size(); ++i)
    {
        threads[i].join();
    }
    ASSERT_EQ(nOk.load(), 8);
    exec.Stop();
}

/// @brief 生命周期加固：执行器析构后，已投递任务仍安全完成（无悬垂指针）。
TEST(NothrowAsync_LifetimeAfterDestroy)
{
    // 空任务构造已私有化，用占位执行器 Submit 创建占位任务（未启动 → 立即无值完成）。
    common::async::CAsyncExecutor execPlaceholder(1);
    common::async::CTask<int> task = execPlaceholder.Submit([]() { return 0; });
    {
        common::async::CAsyncExecutor exec(1);
        ASSERT_TRUE(exec.Start());
        task = exec.Submit([]() { return 7; });
    } // exec 析构：Stop 等待任务完成，句柄仍被 task 持有
    common::async::CTaskResult<int> r = task.Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 7);
}

/// @brief 默认构造为无值（None）；ValueOr 便捷取值。
TEST(NothrowAsync_DefaultAndValueOr)
{
    common::async::CTaskResult<int> def; // 默认无值（None）
    ASSERT_TRUE(!def.HasValue());
    ASSERT_EQ(def.Reason(), common::async::detail::kEndNone);
    ASSERT_EQ(def.ValueOr(-1), -1);

    common::async::CTaskResult<int> ok(5); // 有值（隐式）
    ASSERT_TRUE(ok.HasValue());
    ASSERT_EQ(ok.ValueOr(-1), 5);
}

/// @brief OnSuccess / OnNone 回调（经执行器异步触发，Stop 后确认）。
TEST(NothrowAsync_OnSuccessOnNone)
{
    std::atomic<int> nOk(0);
    std::atomic<int> nNone(0);

    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    common::async::CTask<int> t = exec.Submit([]() { return 9; });
    t.OnSuccess([&nOk](const int& v) { if (v == 9) nOk.fetch_add(1); });
    t.OnNone([&nNone](common::async::detail::CTaskEndReason) { nNone.fetch_add(1); });

    common::async::CTask<int> f =
        exec.Submit([]() -> int { throw std::runtime_error("boom"); }); // 无值
    f.OnSuccess([&nOk](const int&) { nOk.fetch_add(1); });
    f.OnNone([&nNone](common::async::detail::CTaskEndReason) { nNone.fetch_add(1); });

    exec.Stop(); // 等待异步回调（投递到线程池）执行完成
    ASSERT_EQ(nOk.load(), 1);
    ASSERT_EQ(nNone.load(), 1);
}

/// @brief OnSuccess/OnNone 返回 bool：运行中登记成功（true）；执行器不可用 → 失败（false）。
TEST(NothrowAsync_CallbackReturnBool)
{
    // 执行器运行中：任务未完成时注册 → 登记成功（true）
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());
    common::async::CTask<int> t = exec.Submit([]() { return 1; });
    ASSERT_TRUE(t.OnSuccess([](const int&) {}));
    ASSERT_TRUE(t.OnNone([](common::async::detail::CTaskEndReason) {}));
    exec.Stop();

    // 执行器未启动：任务立即无值完成，注册回调 → 投递失败（false）
    common::async::CAsyncExecutor execIdle(1); // 未 Start
    common::async::CTask<int> t2 = execIdle.Submit([]() { return 1; }); // kNotStarted 已完成
    ASSERT_TRUE(!t2.OnSuccess([](const int&) {}));
    ASSERT_TRUE(!t2.OnNone([](common::async::detail::CTaskEndReason) {}));
}

/// @brief 一个任务挂多个 OnSuccess（fan-out），全部触发。
TEST(NothrowAsync_MultipleCallbacks)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());
    std::atomic<int> nA(0), nB(0);
    common::async::CTask<int> t = exec.Submit([]() { return 7; });
    t.OnSuccess([&nA](const int& v) { if (v == 7) nA.fetch_add(1); });
    t.OnSuccess([&nB](const int& v) { if (v == 7) nB.fetch_add(1); });
    exec.Stop(); // 等待回调执行
    ASSERT_EQ(nA.load(), 1);
    ASSERT_EQ(nB.load(), 1);
}

/// @brief 任务已完成后再 Then：续接异步投递，下游正常完成。
TEST(NothrowAsync_ThenOnCompletedTask)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());
    common::async::CTask<int> t = exec.Submit([]() { return 1; });
    common::async::CTaskResult<int> r0 = t.Get(); // 任务已完成
    ASSERT_TRUE(r0.HasValue());
    common::async::CTaskResult<int> r = t.Then([](int n) { return n + 1; }).Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 2);
    exec.Stop();
}

/// @brief 链中变换抛异常 → 无值（kException），后续 Then 跳过。
TEST(NothrowAsync_TransformException)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());
    common::async::CTaskResult<int> r =
        exec.Submit([]() { return 1; })
            .Then([](int) -> int { throw std::runtime_error("transform boom"); })
            .Then([](int n) { return n + 1; }) // 上游异常无值，此步跳过
            .Get();
    ASSERT_TRUE(!r.HasValue());
    ASSERT_EQ(r.Reason(), common::async::detail::kException);
    exec.Stop();
}

/// @brief 一个任务分叉两个 Then：两个下游各自拿到值。
TEST(NothrowAsync_ChainFork)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());
    common::async::CTask<int> t = exec.Submit([]() { return 5; });
    common::async::CTaskResult<int> r1 = t.Then([](int n) { return n + 1; }).Get();
    common::async::CTaskResult<int> r2 = t.Then([](int n) { return n * 2; }).Get();
    ASSERT_TRUE(r1.HasValue());
    ASSERT_EQ(r1.Value(), 6);
    ASSERT_TRUE(r2.HasValue());
    ASSERT_EQ(r2.Value(), 10);
    exec.Stop();
}

/// @brief 停止后 Submit → 视为失败（kNotStarted）。
TEST(NothrowAsync_SubmitAfterStop)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());
    common::async::CTaskResult<int> ok = exec.Submit([]() { return 1; }).Get();
    ASSERT_TRUE(ok.HasValue());
    exec.Stop();
    common::async::CTaskResult<int> r = exec.Submit([]() { return 2; }).Get();
    ASSERT_TRUE(!r.HasValue());
    ASSERT_EQ(r.Reason(), common::async::detail::kNotStarted);
}

/// @brief 任务完成后执行器停止，再 Then → 续接投递失败，下游 kStopped（不阻塞）。
TEST(NothrowAsync_ThenAfterStop)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());
    common::async::CTask<int> t = exec.Submit([]() { return 1; });
    t.Get(); // 任务已完成
    exec.Stop();
    common::async::CTaskResult<int> r = t.Then([](int n) { return n + 1; }).Get();
    ASSERT_TRUE(!r.HasValue());
    ASSERT_EQ(r.Reason(), common::async::detail::kStopped);
}

/// @brief 停止后重新 Start → 正常工作（旧任务隔离）。
TEST(NothrowAsync_RestartExecutor)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());
    common::async::CTaskResult<int> r1 = exec.Submit([]() { return 1; }).Get();
    ASSERT_TRUE(r1.HasValue());
    ASSERT_EQ(r1.Value(), 1);
    exec.Stop();

    ASSERT_TRUE(exec.Start());
    common::async::CTaskResult<int> r2 = exec.Submit([]() { return 2; }).Get();
    ASSERT_TRUE(r2.HasValue());
    ASSERT_EQ(r2.Value(), 2);
    exec.Stop();
}

/// @brief Post：未启动失败；启动后成功且任务执行。
TEST(NothrowAsync_PostBehavior)
{
    common::async::CAsyncExecutor execIdle(1); // 未 Start
    ASSERT_TRUE(!execIdle.Post([]() {}));

    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());
    std::atomic<int> nDone(0);
    ASSERT_TRUE(exec.Post([&nDone]() { nDone.fetch_add(1); }));
    exec.Stop();
    ASSERT_EQ(nDone.load(), 1);
}

/// @brief 长链（10 级 Then）值正确。
TEST(NothrowAsync_DeepChain)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());
    common::async::CTask<int> t = exec.Submit([]() { return 0; });
    for (int i = 1; i <= 10; ++i)
    {
        t = t.Then([i](int n) { return n + i; });
    }
    common::async::CTaskResult<int> r = t.Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 55); // 1+2+...+10
    exec.Stop();
}

/// @brief void 任务被上游终止（异常）→ OnNone（void 版）触发。
TEST(NothrowAsync_VoidOnNone)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());
    std::atomic<int> nNone(0);
    common::async::CTask<void> vt =
        exec.Submit([]() { throw std::runtime_error("void boom"); }); // void 任务无值
    vt.OnNone([&nNone](common::async::detail::CTaskEndReason reason)
    {
        if (reason == common::async::detail::kException)
        {
            nNone.fetch_add(1);
        }
    });
    common::async::CTaskResult<void> r = vt.Get();
    ASSERT_TRUE(!r.HasValue());
    exec.Stop();
    ASSERT_EQ(nNone.load(), 1);
}

/// @brief 任务函数在工作线程执行（不是主线程）。
TEST(NothrowAsync_WorkerThread)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());
    std::thread::id mainId = std::this_thread::get_id();
    std::thread::id workerId;
    common::async::CTaskResult<void> r =
        exec.Submit([&workerId, mainId]()
        {
            workerId = std::this_thread::get_id();
            (void)mainId;
        }).Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_TRUE(workerId != mainId); // 任务在工作线程执行
    exec.Stop();
}
