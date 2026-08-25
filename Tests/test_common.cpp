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
#include "Async/AsyncExecutorNoThrow.h"
#include "Config/Config.h"
#include "Network/Buffer.h"
#include "Thread/ThreadPool.h"
#include "Timer/TimerManager.h"

/// @brief 缓冲区追加/读取/清空。
TEST(Buffer_AppendReadRetrieve)
{
    common::CBuffer buffer;
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
    common::CThreadPool pool(2);
    ASSERT_TRUE(pool.Start());

    std::atomic<int> nCounter(0);
    for (int i = 0; i < 20; ++i)
    {
        ASSERT_TRUE(pool.Submit([&nCounter]() { nCounter.fetch_add(1); }));
    }
    pool.Stop(); // 等待所有已提交任务执行完毕
    ASSERT_EQ(nCounter.load(), 20);
}

/// @brief 定时器一次性触发。
TEST(Timer_OneShotFires)
{
    common::CTimerManager timerManager;
    ASSERT_TRUE(timerManager.Start());

    std::atomic<int> nFired(0);
    common::TimerId nId = timerManager.AddTimer(30, [&nFired]() { nFired.fetch_add(1); });
    ASSERT_TRUE(nId != common::kInvalidTimerId);

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

    common::CConfig config;
    ASSERT_TRUE(config.LoadFile(strPath));
    ASSERT_EQ(config.GetString("name", ""), std::string("ServerA"));
    ASSERT_EQ(config.GetString("missing", "def"), std::string("def"));
    ASSERT_EQ(config.GetInt("port", 0), 9500);
    ASSERT_EQ(config.GetInt("missing", -1), -1);
}

/// @brief 链式异步任务结果。
TEST(Async_SubmitAndGet)
{
    common::CAsyncExecutor executor;
    ASSERT_TRUE(executor.Start());

    int nResult = executor.Submit([]() { return 42; }).Get();
    ASSERT_EQ(nResult, 42);
    executor.Stop();
}

// ==================== 无异常版异步框架（common::nothrow，Option 风格） ====================

/// @brief 链式任务成功（Submit → Then → Then → Get，有值传播）。
TEST(NothrowAsync_ChainGet)
{
    common::nothrow::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    common::nothrow::CTaskResult<int> r =
        exec.Submit([]() { return 3; })
            .Then([](int n) { return n * 2; })
            .Then([](int n) { return n + 1; })
            .Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 7);
    ASSERT_TRUE(static_cast<bool>(r)); // operator bool
    exec.Stop();
}

/// @brief 无值终止沿链传播（上游 None，Then 不执行）。
TEST(NothrowAsync_NonePropagate)
{
    common::nothrow::CTaskResult<int> r =
        common::nothrow::CTask<int>::FromResult(
            common::nothrow::CTaskResult<int>()) // 无值（None）
            .Then([](int n) { return n + 1; })
            .Get();
    ASSERT_TRUE(!r.HasValue());
    ASSERT_EQ(r.Reason(), common::nothrow::detail::kEndNone);
}

/// @brief 任务函数抛出异常 → 转为无值终止（原因 kException）。
TEST(NothrowAsync_ExceptionToNone)
{
    common::nothrow::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    common::nothrow::CTaskResult<int> r =
        exec.Submit([]() -> int { throw std::runtime_error("boom"); }).Get();
    ASSERT_TRUE(!r.HasValue());
    ASSERT_EQ(r.Reason(), common::nothrow::detail::kException);
    exec.Stop();
}

/// @brief 未启动执行器直接 Submit → 无值终止（原因 kNotStarted）。
TEST(NothrowAsync_NotStarted)
{
    common::nothrow::CAsyncExecutor exec(1);
    common::nothrow::CTaskResult<int> r = exec.Submit([]() { return 1; }).Get();
    ASSERT_TRUE(!r.HasValue());
    ASSERT_EQ(r.Reason(), common::nothrow::detail::kNotStarted);
}

/// @brief 链式 flatMap：变换函数返回 CTask，自动平铺为下游结果。
TEST(NothrowAsync_FlatMap)
{
    common::nothrow::CTaskResult<int> r =
        common::nothrow::CTask<int>::FromResult(common::nothrow::CTaskResult<int>(3))
            .Then([](int n) {
                return common::nothrow::CTask<int>::FromResult(
                    common::nothrow::CTaskResult<int>(n * 10));
            })
            .Then([](int n) { return n + 5; })
            .Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 35);
}

/// @brief flatMap 内部任务无值终止 → 终止沿链传播。
TEST(NothrowAsync_FlatMapNonePropagate)
{
    common::nothrow::CTaskResult<int> r =
        common::nothrow::CTask<int>::FromResult(common::nothrow::CTaskResult<int>(1))
            .Then([](int) {
                return common::nothrow::CTask<int>::FromResult(
                    common::nothrow::CTaskResult<int>()); // 内部任务无值
            })
            .Get();
    ASSERT_TRUE(!r.HasValue());
    ASSERT_EQ(r.Reason(), common::nothrow::detail::kEndNone);
}

/// @brief 变换函数直接返回 CTaskResult：有值传播、无值（None）终止。
TEST(NothrowAsync_ThenReturnsResult)
{
    // 变换返回 CTaskResult：有值继续接龙
    common::nothrow::CTaskResult<int> r =
        common::nothrow::CTask<int>::FromResult(common::nothrow::CTaskResult<int>(3))
            .Then([](int n) -> common::nothrow::CTaskResult<int> {
                if (n < 0)
                {
                    return common::nothrow::None; // 无值 → 终止
                }
                return n * 10; // 有值（隐式）
            })
            .Then([](int n) { return n + 5; })
            .Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 35);

    // 变换返回 None → 中途终止，后续 Then 不执行
    common::nothrow::CTaskResult<int> r2 =
        common::nothrow::CTask<int>::FromResult(common::nothrow::CTaskResult<int>(-3))
            .Then([](int n) -> common::nothrow::CTaskResult<int> {
                if (n < 0)
                {
                    return common::nothrow::None;
                }
                return n * 10;
            })
            .Then([](int n) { return n + 5; })
            .Get();
    ASSERT_TRUE(!r2.HasValue());
    ASSERT_EQ(r2.Reason(), common::nothrow::detail::kEndNone);
}

/// @brief 无返回值任务（CTaskResult&lt;void&gt;：完成即 HasValue）。
TEST(NothrowAsync_VoidTask)
{
    common::nothrow::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::atomic<int> nDone(0);
    common::nothrow::CTaskResult<void> r =
        exec.Submit([&nDone]() { nDone.fetch_add(1); }).Get();
    ASSERT_TRUE(r.HasValue()); // void 完成
    ASSERT_EQ(nDone.load(), 1);
    exec.Stop();
}

/// @brief void 任务支持 Then（无参数往下传），可继续链。
TEST(NothrowAsync_VoidThenChain)
{
    common::nothrow::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    common::nothrow::CTaskResult<int> r =
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
    common::nothrow::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::atomic<int> nSteps(0);
    common::nothrow::CTaskResult<void> r =
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
    common::nothrow::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::atomic<int> nConsumed(0);
    common::nothrow::CTaskResult<void> r =
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
    common::nothrow::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    std::vector<common::nothrow::CTask<int> > tasks;
    for (int i = 0; i < 8; ++i)
    {
        tasks.push_back(exec.Submit([i]() { return i * i; }));
    }
    std::atomic<int> nOk(0);
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
    {
        threads.push_back(std::thread([&tasks, i, &nOk]() {
            common::nothrow::CTaskResult<int> r = tasks[i].Get();
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
    common::nothrow::CTask<int> task;
    {
        common::nothrow::CAsyncExecutor exec(1);
        ASSERT_TRUE(exec.Start());
        task = exec.Submit([]() { return 7; });
    } // exec 析构：Stop 等待任务完成，句柄仍被 task 持有
    common::nothrow::CTaskResult<int> r = task.Get();
    ASSERT_TRUE(r.HasValue());
    ASSERT_EQ(r.Value(), 7);
}

/// @brief 默认构造为无值（None）；ValueOr 便捷取值。
TEST(NothrowAsync_DefaultAndValueOr)
{
    common::nothrow::CTaskResult<int> def; // 默认无值（None）
    ASSERT_TRUE(!def.HasValue());
    ASSERT_EQ(def.Reason(), common::nothrow::detail::kEndNone);
    ASSERT_EQ(def.ValueOr(-1), -1);

    common::nothrow::CTaskResult<int> ok(5); // 有值（隐式）
    ASSERT_TRUE(ok.HasValue());
    ASSERT_EQ(ok.ValueOr(-1), 5);
}

/// @brief OnSuccess / OnNone 回调（任务已完成时在注册线程同步触发）。
TEST(NothrowAsync_OnSuccessOnNone)
{
    std::atomic<int> nOk(0);
    std::atomic<int> nNone(0);

    common::nothrow::CTask<int> t = common::nothrow::CTask<int>::FromResult(
        common::nothrow::CTaskResult<int>(9));
    t.OnSuccess([&nOk](const int& v) { if (v == 9) nOk.fetch_add(1); });
    t.OnNone([&nNone](common::nothrow::detail::CTaskEndReason) { nNone.fetch_add(1); });
    ASSERT_EQ(nOk.load(), 1);
    ASSERT_EQ(nNone.load(), 0);

    common::nothrow::CTask<int> f = common::nothrow::CTask<int>::FromResult(
        common::nothrow::CTaskResult<int>()); // 无值
    f.OnSuccess([&nOk](const int&) { nOk.fetch_add(1); });
    f.OnNone([&nNone](common::nothrow::detail::CTaskEndReason) { nNone.fetch_add(1); });
    ASSERT_EQ(nOk.load(), 1);
    ASSERT_EQ(nNone.load(), 1);
}
