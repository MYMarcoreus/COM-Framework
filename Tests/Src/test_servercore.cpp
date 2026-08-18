/// @file test_servercore.cpp
/// ServerCore 单元测试：组件生命周期 / 组件管理器编排 / 事件分发。

#include <atomic>
#include <cstdio>
#include <string>

#include "TestFramework.h"

#include "Component/Component.h"
#include "Component/ComponentManager.h"
#include "Event/EventDispatcher.h"

namespace {

/// @brief 用于记录生命周期调用序的测试组件。
class CTestComponent : public sc::CComponent
{
public:
    CTestComponent() : m_nInit(0), m_nStart(0), m_nStop(0), m_nShutdown(0) {}

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
        return "test-component";
    }

    int m_nInit;
    int m_nStart;
    int m_nStop;
    int m_nShutdown;
};

/// @brief 测试组件接口标识。
inline const sc::InterfaceId& IID_TestComponent()
{
    static const sc::InterfaceId iid = "sc::TestComponent";
    return iid;
}

} // namespace

/// @brief 引用计数与接口查询。
TEST(Component_RefCountAndQuery)
{
    sc::CComponent* pComponent = new sc::CComponent();
    ASSERT_EQ(pComponent->AddRef(), 2u); // 创建时 1，AddRef 后 2
    ASSERT_EQ(pComponent->Release(), 1u);

    void* ppv = nullptr;
    ASSERT_TRUE(pComponent->QueryInterface(sc::IID_IUnknown(), &ppv));
    ASSERT_TRUE(ppv != nullptr);
    ASSERT_EQ(pComponent->Release(), 0u); // 归零销毁
}

/// @brief 组件管理器生命周期编排。
TEST(ComponentManager_Lifecycle)
{
    sc::CComponentManager manager;
    CTestComponent* pComponent = new CTestComponent();
    ASSERT_TRUE(manager.RegisterComponent(IID_TestComponent(), pComponent));
    pComponent->Release(); // 管理器已持有引用

    ASSERT_EQ(manager.Size(), static_cast<size_t>(1));
    ASSERT_TRUE(manager.GetComponent(IID_TestComponent()) != nullptr);
    ASSERT_TRUE(manager.InitializeAll());
    ASSERT_TRUE(manager.StartAll());
    ASSERT_EQ(pComponent->m_nInit, 1);
    ASSERT_EQ(pComponent->m_nStart, 1);

    manager.StopAll();
    manager.ShutdownAll();
    ASSERT_EQ(pComponent->m_nStop, 1);
    ASSERT_EQ(pComponent->m_nShutdown, 1);

    std::string strReport = manager.StatusReport();
    ASSERT_TRUE(strReport.find("test-component") != std::string::npos);
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
