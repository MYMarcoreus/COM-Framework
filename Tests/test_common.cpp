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

// ==================== 无异常版异步框架（common::nothrow） ====================

/// @brief 链式任务成功（Submit → Then → Then → Get）。
TEST(NothrowAsync_ChainGet)
{
    common::nothrow::CAsyncExecutor<> exec(2);
    ASSERT_TRUE(exec.Start());

    common::nothrow::CTaskResult<int> r =
        exec.Submit([]() { return 3; })
            .Then([](int n) { return n * 2; })
            .Then([](int n) { return n + 1; })
            .Get();
    ASSERT_TRUE(r.Ok());
    ASSERT_EQ(r.Value(), 7);
    ASSERT_TRUE(static_cast<bool>(r)); // operator bool
    exec.Stop();
}

/// @brief 错误沿链传播（业务自定义错误码，Then 不执行）。
TEST(NothrowAsync_ErrorPropagate)
{
    common::nothrow::CTaskResult<int> r =
        common::nothrow::CTask<int>::FromResult(
            common::nothrow::CTaskResult<int>::Failure(1001, "业务错误"))
            .Then([](int n) { return n + 1; })
            .Get();
    ASSERT_TRUE(r.Failed());
    ASSERT_EQ(r.Error().nCode, 1001);
    ASSERT_EQ(r.Error().strMessage, std::string("业务错误"));
}

/// @brief 任务函数抛出异常 → 转为 kTaskFailed。
TEST(NothrowAsync_ExceptionToError)
{
    common::nothrow::CAsyncExecutor<> exec(1);
    ASSERT_TRUE(exec.Start());

    common::nothrow::CTaskResult<int> r =
        exec.Submit([]() -> int { throw std::runtime_error("boom"); }).Get();
    ASSERT_TRUE(r.Failed());
    ASSERT_EQ(r.Error().nCode, common::nothrow::kTaskFailed);
    ASSERT_EQ(r.Error().strMessage, std::string("boom"));
    exec.Stop();
}

/// @brief 未启动执行器直接 Submit → kExecutorNotStarted。
TEST(NothrowAsync_NotStarted)
{
    common::nothrow::CAsyncExecutor<> exec(1);
    common::nothrow::CTaskResult<int> r = exec.Submit([]() { return 1; }).Get();
    ASSERT_TRUE(r.Failed());
    ASSERT_EQ(r.Error().nCode, common::nothrow::kExecutorNotStarted);
}

/// @brief 链式 flatMap：变换函数返回 CTask，自动平铺为下游结果。
TEST(NothrowAsync_FlatMap)
{
    common::nothrow::CTaskResult<int> r =
        common::nothrow::CTask<int>::FromResult(
            common::nothrow::CTaskResult<int>::Success(3))
            .Then([](int n) {
                return common::nothrow::CTask<int>::FromResult(
                    common::nothrow::CTaskResult<int>::Success(n * 10));
            })
            .Then([](int n) { return n + 5; })
            .Get();
    ASSERT_TRUE(r.Ok());
    ASSERT_EQ(r.Value(), 35);
}

/// @brief flatMap 内部任务失败 → 错误沿链传播。
TEST(NothrowAsync_FlatMapErrorPropagate)
{
    common::nothrow::CTaskResult<int> r =
        common::nothrow::CTask<int>::FromResult(
            common::nothrow::CTaskResult<int>::Success(1))
            .Then([](int) {
                return common::nothrow::CTask<int>::FromResult(
                    common::nothrow::CTaskResult<int>::Failure(2002, "内部失败"));
            })
            .Get();
    ASSERT_TRUE(r.Failed());
    ASSERT_EQ(r.Error().nCode, 2002);
    ASSERT_EQ(r.Error().strMessage, std::string("内部失败"));
}

/// @brief 无返回值任务（CTaskResult&lt;void&gt;）。
TEST(NothrowAsync_VoidTask)
{
    common::nothrow::CAsyncExecutor<> exec(1);
    ASSERT_TRUE(exec.Start());

    std::atomic<int> nDone(0);
    common::nothrow::CTaskResult<void> r =
        exec.Submit([&nDone]() { nDone.fetch_add(1); }).Get();
    ASSERT_TRUE(r.Ok());
    ASSERT_EQ(nDone.load(), 1);
    exec.Stop();
}

/// @brief 并发多任务 + 多线程 Get。
TEST(NothrowAsync_ConcurrentGet)
{
    common::nothrow::CAsyncExecutor<> exec(4);
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
            if (r.Ok() && r.Value() == i * i)
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
        common::nothrow::CAsyncExecutor<> exec(1);
        ASSERT_TRUE(exec.Start());
        task = exec.Submit([]() { return 7; });
    } // exec 析构：Stop 等待任务完成，句柄仍被 task 持有
    common::nothrow::CTaskResult<int> r = task.Get();
    ASSERT_TRUE(r.Ok());
    ASSERT_EQ(r.Value(), 7);
}

/// @brief 默认构造为失败态；ValueOr 便捷取值。
TEST(NothrowAsync_DefaultAndValueOr)
{
    common::nothrow::CTaskResult<int> def; // 默认失败态（kTaskFailed + uninitialized）
    ASSERT_TRUE(def.Failed());
    ASSERT_EQ(def.Error().nCode, common::nothrow::kTaskFailed);
    ASSERT_EQ(def.ValueOr(-1), -1);

    common::nothrow::CTaskResult<int> ok =
        common::nothrow::CTaskResult<int>::Success(5);
    ASSERT_TRUE(ok.Ok());
    ASSERT_EQ(ok.ValueOr(-1), 5);
}

/// @brief OnSuccess / OnFailure 回调（任务已完成时在注册线程同步触发）。
TEST(NothrowAsync_OnSuccessOnFailure)
{
    std::atomic<int> nOk(0);
    std::atomic<int> nFail(0);

    common::nothrow::CTask<int> t = common::nothrow::CTask<int>::FromResult(
        common::nothrow::CTaskResult<int>::Success(9));
    t.OnSuccess([&nOk](const int& v) { if (v == 9) nOk.fetch_add(1); });
    t.OnFailure([&nFail](const common::nothrow::CTaskError&) { nFail.fetch_add(1); });
    ASSERT_EQ(nOk.load(), 1);
    ASSERT_EQ(nFail.load(), 0);

    common::nothrow::CTask<int> f = common::nothrow::CTask<int>::FromResult(
        common::nothrow::CTaskResult<int>::Failure(3003, "x"));
    f.OnSuccess([&nOk](const int&) { nOk.fetch_add(1); });
    f.OnFailure([&nFail](const common::nothrow::CTaskError&) { nFail.fetch_add(1); });
    ASSERT_EQ(nOk.load(), 1);
    ASSERT_EQ(nFail.load(), 1);
}

/// @brief CTaskResult 支持自定义错误类型（类似 std::expected<T, E>）。
TEST(NothrowAsync_CustomErrorType)
{
    struct MyError
    {
        int nCode;
        std::string strMsg;
    };

    // 自定义错误类型 + 值
    common::nothrow::CTaskResult<int, MyError> ok =
        common::nothrow::CTaskResult<int, MyError>::Success(5);
    ASSERT_TRUE(ok.Ok());
    ASSERT_EQ(ok.Value(), 5);

    // 自定义错误类型 + 失败（通用 Failure(const TError&)）
    common::nothrow::CTaskResult<int, MyError> fail =
        common::nothrow::CTaskResult<int, MyError>::Failure(MyError{9001, "自定义错误"});
    ASSERT_TRUE(fail.Failed());
    ASSERT_EQ(fail.Error().nCode, 9001);
    ASSERT_EQ(fail.Error().strMsg, std::string("自定义错误"));

    // void 特化 + 自定义错误类型
    common::nothrow::CTaskResult<void, MyError> vfail =
        common::nothrow::CTaskResult<void, MyError>::Failure(MyError{9002, "void 失败"});
    ASSERT_TRUE(vfail.Failed());
    ASSERT_EQ(vfail.Error().nCode, 9002);

    // 默认 TError = CTaskError 时，(错误码, 消息) 便捷工厂仍可用
    common::nothrow::CTaskResult<int> defaultErr =
        common::nothrow::CTaskResult<int>::Failure(1001, "业务错误");
    ASSERT_TRUE(defaultErr.Failed());
    ASSERT_EQ(defaultErr.Error().nCode, 1001);
}
