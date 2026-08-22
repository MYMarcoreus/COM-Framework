/// @file test_infra.cpp
/// ServerCore 增强功能单元测试：
///   依赖拓扑排序 / 接口多实例注册 / 配置热加载 / 日志文件滚动 / TCP 连接数上限 /
///   指标注册表 / 连接级上下文 / 异步事件分发。

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "TestFramework.h"

#include "Event/EventDispatcher.h"
#include "Infra/AsyncExecutorModule.h"
#include "Infra/ConfigReloadModule.h"
#include "Infra/ConfigModule.h"
#include "Log/Logger.h"
#include "Module/Module.h"
#include "Module/ModuleManager.h"
#include "Network/NetworkModule.h"
#include "Network/TcpServer.h"
#include "Observability/MetricsModule.h"

namespace {

/// @brief 记录模块初始化顺序的辅助。
struct OrderRecorder
{
    static std::vector<std::string> s_order;

    static void Record(const std::string& strName)
    {
        s_order.push_back(strName);
    }

    static void Clear()
    {
        s_order.clear();
    }
};
std::vector<std::string> OrderRecorder::s_order;

/// @brief 用于记录初始化顺序的测试模块。
class COrderModule : public sc::CModule
{
public:
    explicit COrderModule(const char* strName, const sc::InterfaceId* pDepIid = nullptr)
        : sc::CModule(strName)
    {
        if (pDepIid != nullptr)
        {
            AddDependency(*pDepIid);
        }
    }

    bool Initialize(const sc::CResolveContext& /*ctx*/) override
    {
        OrderRecorder::Record(GetName());
        return true;
    }

    // 生命周期：拓扑排序测试只关注初始化顺序，其余阶段空实现。
    bool Start() override
    {
        return true;
    }

    void Stop() override
    {
    }

    void Shutdown() override
    {
    }
};

/// @brief 建立到本机端口的 TCP 连接；失败返回 -1。
int ConnectTo(std::uint16_t nPort)
{
    int nFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (nFd < 0)
    {
        return -1;
    }
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(nPort);
    if (::connect(nFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        ::close(nFd);
        return -1;
    }
    return nFd;
}

} // namespace

/// @brief 依赖拓扑排序：被依赖的接口模块先于依赖者初始化，即使注册顺序相反。
TEST(ModuleManager_TopologicalOrder)
{
    OrderRecorder::Clear();
    sc::CModuleManager manager;
    // 接口 iidDep 由模块 A 提供；模块 B 声明依赖它。
    sc::InterfaceId iidDep("sc::TestDep", "11111111-2222-3333-4444-555555555555");

    // 故意先注册依赖者 B，再注册被依赖者 A（注册顺序与依赖方向相反）。
    ASSERT_TRUE(manager.RegisterModule(new COrderModule("b", &iidDep)));
    ASSERT_TRUE(manager.RegisterModule(iidDep, new COrderModule("a", nullptr)));

    ASSERT_TRUE(manager.InitializeAll());
    ASSERT_EQ(OrderRecorder::s_order.size(), static_cast<size_t>(2));
    ASSERT_EQ(OrderRecorder::s_order[0], std::string("a")); // 被依赖者先初始化
    ASSERT_EQ(OrderRecorder::s_order[1], std::string("b"));
}

/// @brief 接口多实例注册：同一接口注册多个模块，支持按接口查询全部 / 计数 / 取首个。
TEST(ModuleManager_MultiInstance)
{
    sc::CModuleManager manager;
    sc::InterfaceId iid("sc::TestMulti", "66666666-7777-8888-9999-aaaaaaaaaaaa");

    sc::CModule* pM1 = new COrderModule("m1", nullptr);
    sc::CModule* pM2 = new COrderModule("m2", nullptr);
    ASSERT_TRUE(manager.RegisterModule(iid, pM1));
    ASSERT_TRUE(manager.RegisterModule(iid, pM2)); // 同一接口可再次注册

    ASSERT_EQ(manager.ModuleCountByIid(iid), static_cast<size_t>(2));
    std::vector<sc::IModule*> vec = manager.GetModulesByIid(iid);
    ASSERT_EQ(vec.size(), static_cast<size_t>(2));
    ASSERT_TRUE(vec[0] == pM1);
    ASSERT_TRUE(vec[1] == pM2);
    // GetModuleByIid 返回首个实例（兼容单实例调用方）
    ASSERT_TRUE(manager.GetModuleByIid(iid) == pM1);

    // 反注册一个实例后剩余一个
    ASSERT_TRUE(manager.UnregisterModuleByIid(iid));
    ASSERT_EQ(manager.ModuleCountByIid(iid), static_cast<size_t>(1));
    ASSERT_TRUE(manager.GetModuleByIid(iid) == pM2);
}

/// @brief 配置热加载：IConfig::ReloadIfChanged 感知文件变更并整体重载。
TEST(Config_ReloadIfChanged)
{
    std::string strPath = "/tmp/config_reload_" +
                          std::to_string(static_cast<long long>(::getpid())) + ".ini";
    std::ofstream(strPath.c_str()) << "port = 1000\n";

    sc::CConfigModule config;
    ASSERT_TRUE(config.LoadFile(strPath));
    ASSERT_EQ(config.GetInt("port", 0), 1000);
    ASSERT_TRUE(!config.ReloadIfChanged()); // 未变更不重载

    // 修改文件内容并等待 mtime 变化（秒级精度）
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::ofstream(strPath.c_str()) << "port = 2000\n";
    ASSERT_TRUE(config.ReloadIfChanged());
    ASSERT_EQ(config.GetInt("port", 0), 2000);

    std::remove(strPath.c_str());
}

/// @brief 日志文件滚动：超过大小上限后生成 .1 备份。
TEST(Logger_FileRotation)
{
    std::string strPath = "/tmp/logger_rot_" +
                          std::to_string(static_cast<long long>(::getpid())) + ".log";
    common::CLogger& logger = common::CLogger::Instance();
    logger.OpenFile(strPath);
    logger.SetMaxFileSize(100); // 100 字节触发滚动

    for (int i = 0; i < 20; ++i)
    {
        logger.Info("rotation test line " + std::to_string(i));
    }

    struct stat st;
    bool bBackup = (::stat((strPath + ".1").c_str(), &st) == 0);
    ASSERT_TRUE(bBackup);

    // 恢复单例状态并清理临时文件
    logger.SetMaxFileSize(0);
    logger.OpenFile("/dev/null");
    std::remove(strPath.c_str());
    std::remove((strPath + ".1").c_str());
    std::remove((strPath + ".2").c_str());
}

/// @brief TCP 连接数上限：达到上限后新连接被直接关闭，不触发 Accept 回调。
TEST(Network_ConnectionLimit)
{
    common::CTcpServer server;
    server.SetMaxConnections(1);
    std::uint16_t nPort = static_cast<std::uint16_t>(20000 + (::getpid() % 5000));

    std::atomic<int> nAccept(0);
    if (!server.Start(nPort,
            [&nAccept](common::ConnectionId, const std::string&) { nAccept.fetch_add(1); },
            [](common::ConnectionId, const char*, size_t) {},
            [](common::ConnectionId) {}))
    {
        ASSERT_TRUE(false); // 端口被占用（测试环境偶然冲突）
    }

    // 第一个连接：应正常建立并触发 Accept
    int nFd1 = ConnectTo(nPort);
    ASSERT_TRUE(nFd1 >= 0);
    for (int i = 0; i < 100 && nAccept.load() < 1; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(nAccept.load(), 1);
    ASSERT_EQ(server.ConnectionCount(), static_cast<size_t>(1));

    // 第二个连接：达到上限，服务器端拒绝（不触发 Accept）
    int nFd2 = ConnectTo(nPort);
    ASSERT_TRUE(nFd2 >= 0); // 内核完成握手，但服务器端会直接关闭
    for (int i = 0; i < 100 && nAccept.load() < 2; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(nAccept.load(), 1); // 上限之外的连接不进入 Accept
    ASSERT_EQ(server.ConnectionCount(), static_cast<size_t>(1));

    ::close(nFd1);
    ::close(nFd2);
    server.Stop();
}

/// @brief 配置热加载广播：修改配置文件后，CConfigReloadModule 检测变更并发布事件。
TEST(ConfigReloadModule_Broadcast)
{
    std::string strPath = "/tmp/cfg_reload_evt_" +
                          std::to_string(static_cast<long long>(::getpid())) + ".ini";
    std::ofstream(strPath.c_str()) << "key = 1\n";

    sc::CModuleManager manager;
    sc::CConfigModule* pConfig = new sc::CConfigModule();
    pConfig->LoadFile(strPath);
    ASSERT_TRUE(manager.RegisterModule(sc::IID_IConfig(), pConfig));
    ASSERT_TRUE(manager.RegisterModule(sc::IID_IEventDispatcher(), new sc::CEventDispatcher()));
    ASSERT_TRUE(manager.RegisterModule(new sc::CConfigReloadModule(200)));

    // 订阅 config.reloaded 事件
    sc::IEventDispatcher* pIface =
        manager.Resolve<sc::IEventDispatcher>(sc::IID_IEventDispatcher());
    ASSERT_TRUE(pIface != nullptr);
    std::atomic<int> nEvents(0);
    sc::SubscriptionId nSubId = pIface->Subscribe(sc::events::kConfigReloaded,
        [&nEvents](const sc::Event&) { nEvents.fetch_add(1); });
    ASSERT_TRUE(nSubId != sc::kInvalidSubscriptionId);

    ASSERT_TRUE(manager.InitializeAll());
    ASSERT_TRUE(manager.StartAll());

    // 等待 mtime 变化后修改配置文件，再等待重载周期触发事件
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::ofstream(strPath.c_str()) << "key = 2\n";
    for (int i = 0; i < 300 && nEvents.load() == 0; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(nEvents.load() >= 1);

    manager.StopAll();
    manager.ShutdownAll();
    pIface->Unsubscribe(nSubId);
    std::remove(strPath.c_str());
}

/// @brief 指标注册表：计数器自增 / 仪表设置 / 快照 / 查询。
TEST(Metrics_IncGaugeAndSnapshot)
{
    sc::CModuleManager manager;
    ASSERT_TRUE(manager.RegisterModule(sc::IID_IMetrics(), new sc::CMetricsModule()));
    sc::IMetrics* pMetrics = manager.Resolve<sc::IMetrics>(sc::IID_IMetrics());
    ASSERT_TRUE(pMetrics != nullptr);

    // 计数器自增
    pMetrics->Inc("network.accepted");
    pMetrics->Inc("network.accepted");
    pMetrics->Inc("demo.msgs", 3);
    // 仪表设置
    pMetrics->SetGauge("network.conns", 5);

    ASSERT_TRUE(pMetrics->Get("network.accepted") == 2);
    ASSERT_TRUE(pMetrics->Get("demo.msgs") == 3);
    ASSERT_TRUE(pMetrics->Get("network.conns") == 5);
    ASSERT_TRUE(pMetrics->Get("missing") == 0);

    // 快照按名称排序
    std::vector<sc::MetricSnapshot> vecSnapshot = pMetrics->Snapshot();
    ASSERT_TRUE(vecSnapshot.size() == 3);
    ASSERT_TRUE(vecSnapshot[0].strName == "demo.msgs");
    ASSERT_TRUE(vecSnapshot[1].strName == "network.accepted");
    ASSERT_TRUE(vecSnapshot[2].kind == sc::MetricKind::kGauge);
    manager.Clear();
}

/// @brief 连接级上下文：Attach / GetAttached / Detach 全流程。
TEST(Network_ConnectionContext)
{
    sc::CModuleManager manager;
    ASSERT_TRUE(manager.RegisterModule(sc::IID_INetwork(), new sc::CNetworkModule()));
    sc::INetwork* pIface = manager.Resolve<sc::INetwork>(sc::IID_INetwork());
    ASSERT_TRUE(pIface != nullptr);

    int nCtx = 42;
    // 首次挂载返回空
    ASSERT_TRUE(pIface->Attach(1, &nCtx) == nullptr);
    // 取回
    ASSERT_TRUE(pIface->GetAttached(1) == &nCtx);
    // 不存在的连接返回空
    ASSERT_TRUE(pIface->GetAttached(99) == nullptr);
    // 重复挂载返回旧值
    int nCtx2 = 43;
    ASSERT_TRUE(pIface->Attach(1, &nCtx2) == &nCtx);
    // 移除并返回
    ASSERT_TRUE(pIface->Detach(1) == &nCtx2);
    ASSERT_TRUE(pIface->GetAttached(1) == nullptr);
    ASSERT_TRUE(pIface->Detach(99) == nullptr);
    manager.Clear();
}

/// @brief 异步事件分发：PublishAsync 将事件投递到异步执行器线程处理。
TEST(EventDispatcher_PublishAsync)
{
    sc::CModuleManager manager;
    ASSERT_TRUE(manager.RegisterModule(sc::IID_IAsyncExecutor(), new sc::CAsyncExecutorModule(1)));
    ASSERT_TRUE(manager.RegisterModule(sc::IID_IEventDispatcher(), new sc::CEventDispatcher()));

    sc::IEventDispatcher* pIface =
        manager.Resolve<sc::IEventDispatcher>(sc::IID_IEventDispatcher());
    ASSERT_TRUE(pIface != nullptr);

    std::atomic<int> nAsync(0);
    std::atomic<int> nSync(0);
    pIface->Subscribe("async.test", [&nAsync](const sc::Event&) { nAsync.fetch_add(1); });
    pIface->Subscribe("sync.test", [&nSync](const sc::Event&) { nSync.fetch_add(1); });

    ASSERT_TRUE(manager.InitializeAll());
    ASSERT_TRUE(manager.StartAll());

    // 异步发布：返回快照的订阅者数量，事件在工作线程执行
    ASSERT_TRUE(pIface->PublishAsync("async.test", "hello", 5) == 1);
    // 同步发布
    ASSERT_TRUE(pIface->Publish("sync.test", "hi", 2) == 1);
    ASSERT_TRUE(nSync.load() == 1);

    // 等待异步执行完成
    for (int i = 0; i < 200 && nAsync.load() == 0; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(nAsync.load() == 1);

    manager.StopAll();
    manager.ShutdownAll();
    manager.Clear();
}
