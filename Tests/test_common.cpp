/// @file test_common.cpp
/// Common 基础库单元测试：CBuffer / CThreadPool / CTimerManager / CConfig。
///
/// 异步框架（common::async）用例集中在 Tests/test_async_chain.cpp。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
    #include <pthread.h>  // pthread_getname_np：读线程名（线程池起名的用例用）。
#endif

#include "Config/Config.h"
#include "Network/Buffer.h"
#include "TestFramework.h"
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
        ASSERT_TRUE(pool.Submit(
            [&nCounter]()
            {
                nCounter.fetch_add(1);
            }));
    }
    pool.Stop();  // 等待所有已提交任务执行完毕
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
        ASSERT_TRUE(pool.Submit(
            [&nActive, &nPeak, &nDone]()
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
    pool.Stop();  // 等待全部完成
    ASSERT_EQ(nDone.load(), kTasks);
    ASSERT_TRUE(nPeak.load() >= 4);  // 8 线程下突发应显著并行（修复前可能退化为 1）
}

/// @brief 定时器一次性触发。
TEST(Timer_OneShotFires)
{
    common::timer::CTimerManager timerManager;
    ASSERT_TRUE(timerManager.Start());

    std::atomic<int> nFired(0);
    common::timer::TimerId nId = timerManager.AddTimer(30,
        [&nFired]()
        {
            nFired.fetch_add(1);
        });
    ASSERT_TRUE(nId != common::timer::kInvalidTimerId);

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    ASSERT_EQ(nFired.load(), 1);
}

/// @brief 配置读取字符串与整数。
TEST(Config_LoadAndGet)
{
    const char* strPath = "/tmp/test_config_common.ini";
    std::ofstream ofs(strPath);
    ofs << "name = ServerTemplate\n"
        << "port = 9500\n";
    ofs.close();

    common::config::CConfig config;
    ASSERT_TRUE(config.LoadFile(strPath));
    ASSERT_EQ(config.GetString("name", ""), std::string("ServerTemplate"));
    ASSERT_EQ(config.GetString("missing", "def"), std::string("def"));
    ASSERT_EQ(config.GetInt("port", 0), 9500);
    ASSERT_EQ(config.GetInt("missing", -1), -1);
}

/// @brief 线程池给 worker 线程起名（调试用：gdb 的 `info threads` / htop 里可辨认）。
///
/// 在任务里读**自己**的线程名（Linux 的 `pthread_getname_np`），断言它形如 `<池名>-<序号>`。
/// 非 Linux 平台不设线程名，这个用例只验证「不起名不影响功能」。
TEST(ThreadPool_WorkerThreadName)
{
    common::thread::CThreadPool pool(2, "namedpool");
    ASSERT_EQ(pool.Name(), std::string("namedpool"));
    ASSERT_TRUE(pool.Start());

    std::mutex mutex;
    std::vector<std::string> vecNames;
    std::atomic<int> nDone(0);
    for (int i = 0; i < 10; ++i)
    {
        ASSERT_TRUE(pool.Submit(
            [&mutex, &vecNames, &nDone]()
            {
#if defined(__linux__)
                char szName[32] = {0};
                pthread_getname_np(pthread_self(), szName, sizeof(szName));
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    vecNames.push_back(std::string(szName));
                }
#endif
                nDone.fetch_add(1);
            }));
    }
    pool.Stop();  // 等所有任务跑完
    ASSERT_EQ(nDone.load(), 10);

#if defined(__linux__)
    ASSERT_TRUE(!vecNames.empty());
    for (size_t i = 0; i < vecNames.size(); ++i)
    {
        // 形如 `namedpool-0` / `namedpool-1`（池名 + 序号）。
        ASSERT_TRUE(vecNames[i].compare(0, 9, "namedpool") == 0);
        ASSERT_TRUE(vecNames[i].size() > 9 && vecNames[i][9] == '-');
    }
#endif
}

/// @brief 未命名的线程池照常工作（不起线程名 = 保持系统默认，不影响任何行为）。
TEST(ThreadPool_UnnamedStillWorks)
{
    common::thread::CThreadPool pool(1);
    ASSERT_TRUE(pool.Name().empty());
    ASSERT_TRUE(pool.Start());

    std::atomic<int> nCounter(0);
    ASSERT_TRUE(pool.Submit(
        [&nCounter]()
        {
            nCounter.fetch_add(1);
        }));
    pool.Stop();
    ASSERT_EQ(nCounter.load(), 1);
}
