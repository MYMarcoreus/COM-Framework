/// @file test_common.cpp
/// Common 基础库单元测试：CBuffer / CThreadPool / CTimerManager / CConfig / CAsyncExecutor。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>

#include "TestFramework.h"

#include "Async/AsyncExecutor.h"
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
