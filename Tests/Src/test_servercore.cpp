/// @file test_servercore.cpp
/// ServerCore 单元测试：模块生命周期 / 模块管理器编排 / 事件分发。

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "TestFramework.h"

#include "Event/EventDispatcher.h"
#include "Module/Module.h"
#include "Module/ModuleManager.h"

namespace {

/// @brief 用于记录生命周期调用序的测试模块。
class CTestModule : public sc::CModule
{
public:
    CTestModule() : sc::CModule("test"), m_nInit(0), m_nStart(0), m_nStop(0), m_nShutdown(0) {}

    bool Initialize() override
    {
        ++m_nInit;
        return true;
    }

    bool Start() override
    {
        ++m_nStart;
        return true;
    }

    void Stop() override
    {
        ++m_nStop;
    }

    void Shutdown() override
    {
        ++m_nShutdown;
    }

    std::string GetStatus() const override
    {
        return "test-module";
    }

    int m_nInit;
    int m_nStart;
    int m_nStop;
    int m_nShutdown;
};

/// @brief 测试模块接口标识。
inline const sc::InterfaceId& IID_TestModule()
{
    static const sc::InterfaceId iid = "sc::TestModule";
    return iid;
}

} // namespace

/// @brief 引用计数与接口查询。
TEST(Module_RefCountAndQuery)
{
    sc::CModule* pModule = new sc::CModule("test");
    ASSERT_EQ(pModule->AddRef(), 2u); // 创建时 1，AddRef 后 2
    ASSERT_EQ(pModule->Release(), 1u);

    void* ppv = nullptr;
    ASSERT_TRUE(pModule->QueryInterface(sc::IID_IUnknown(), &ppv));
    ASSERT_TRUE(ppv != nullptr);
    ASSERT_EQ(pModule->Release(), 0u); // 归零销毁
}

/// @brief 模块管理器生命周期编排。
TEST(ModuleManager_Lifecycle)
{
    sc::CModuleManager manager;
    CTestModule* pModule = new CTestModule();
    ASSERT_TRUE(manager.RegisterModule(IID_TestModule(), pModule));
    pModule->Release(); // 管理器已持有引用

    ASSERT_EQ(manager.Size(), static_cast<size_t>(1));
    ASSERT_TRUE(manager.GetModuleByIid(IID_TestModule()) != nullptr);
    ASSERT_TRUE(manager.InitializeAll());
    ASSERT_TRUE(manager.StartAll());
    ASSERT_EQ(pModule->m_nInit, 1);
    ASSERT_EQ(pModule->m_nStart, 1);

    manager.StopAll();
    manager.ShutdownAll();
    ASSERT_EQ(pModule->m_nStop, 1);
    ASSERT_EQ(pModule->m_nShutdown, 1);

    std::string strReport = manager.StatusReport();
    ASSERT_TRUE(strReport.find("test-module") != std::string::npos);
}

/// @brief 事件订阅与发布。
TEST(EventDispatcher_SubscribePublish)
{
    sc::CEventDispatcher dispatcher;
    std::atomic<int> nCount(0);
    sc::SubscriptionId nId = dispatcher.Subscribe(
        "evt.test", [&nCount](const sc::Event&) { nCount.fetch_add(1); });
    ASSERT_TRUE(nId != sc::kInvalidSubscriptionId);

    dispatcher.Publish("evt.test", nullptr, 0);
    dispatcher.Publish("evt.test", nullptr, 0);
    ASSERT_EQ(nCount.load(), 2);

    ASSERT_TRUE(dispatcher.Unsubscribe(nId));
    dispatcher.Publish("evt.test", nullptr, 0);
    ASSERT_EQ(nCount.load(), 2); // 取消订阅后不再触发
}

/// @brief 模块状态随生命周期变化（状态查询下沉到模块）。
TEST(Module_StateQuery)
{
    sc::CModuleManager manager;
    CTestModule* pModule = new CTestModule();
    ASSERT_TRUE(manager.RegisterModule(pModule)); // 按名字 "test" 注册
    pModule->Release();

    sc::IModule* pIface = manager.GetModule("test");
    ASSERT_TRUE(pIface != nullptr);
    ASSERT_EQ(pIface->GetState(), sc::ModuleState::kCreated);

    ASSERT_TRUE(manager.InitializeAll());
    ASSERT_EQ(pIface->GetState(), sc::ModuleState::kInitialized);
    ASSERT_TRUE(manager.StartAll());
    ASSERT_EQ(pIface->GetState(), sc::ModuleState::kStarted);

    manager.StopAll();
    ASSERT_EQ(pIface->GetState(), sc::ModuleState::kStopped);
    manager.ShutdownAll();
    ASSERT_EQ(pIface->GetState(), sc::ModuleState::kShutdown);
}

/// @brief 按名字与按接口双注册共存，HasModule 查询。
TEST(ModuleManager_DualRegister)
{
    sc::CModuleManager manager;
    CTestModule* pByName = new CTestModule();
    ASSERT_TRUE(manager.RegisterModule(pByName)); // 按名字 "test"
    pByName->Release();

    CTestModule* pByIid = new CTestModule();
    ASSERT_TRUE(manager.RegisterModule(IID_TestModule(), pByIid)); // 按接口
    pByIid->Release();

    ASSERT_EQ(manager.Size(), static_cast<size_t>(2));
    ASSERT_TRUE(manager.HasModule("test"));
    ASSERT_TRUE(manager.HasModuleByIid(IID_TestModule()));
    ASSERT_TRUE(manager.GetModule("test") != nullptr);
    ASSERT_TRUE(manager.GetModuleByIid(IID_TestModule()) != nullptr);
}

/// @brief 模块快照（名称/接口/状态/描述）。
TEST(ModuleManager_Snapshot)
{
    sc::CModuleManager manager;
    CTestModule* pModule = new CTestModule();
    ASSERT_TRUE(manager.RegisterModule(IID_TestModule(), pModule));
    pModule->Release();
    ASSERT_TRUE(manager.InitializeAll());
    ASSERT_TRUE(manager.StartAll());

    std::vector<sc::ModuleSnapshot> vecSnapshot = manager.Snapshot();
    ASSERT_EQ(vecSnapshot.size(), static_cast<size_t>(1));
    ASSERT_EQ(vecSnapshot[0].strIid, std::string("sc::TestModule"));
    ASSERT_EQ(vecSnapshot[0].strName, std::string("test"));
    ASSERT_EQ(vecSnapshot[0].state, sc::ModuleState::kStarted);
    ASSERT_EQ(vecSnapshot[0].strStatus, std::string("test-module"));
}
