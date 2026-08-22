/// @file test_servercore.cpp
/// ServerCore 单元测试：模块生命周期 / 模块管理器编排 / 事件分发。

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "TestFramework.h"

#include "Event/EventDispatcher.h"
#include "Message/MessageRouter.h"
#include "Module/Module.h"
#include "Module/ModuleManager.h"

namespace {

/// @brief 用于记录生命周期调用序的测试模块。
class CTestModule : public sc::CModule
{
public:
    explicit CTestModule(const char* strName = "test")
        : sc::CModule(strName), m_nInit(0), m_nStart(0), m_nStop(0), m_nShutdown(0) {}

    bool Initialize(const sc::CResolveContext& /*ctx*/) override
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
    static const sc::InterfaceId iid("sc::TestModule", "ee127b1b-a71c-4cad-8f02-39285975b72f");
    return iid;
}

/// @brief 测试接口标识（普通对象接口视图）。
inline const sc::InterfaceId& IID_ITestRefObj()
{
    static const sc::InterfaceId iid("sc::ITestRefObj", "0d2e1f2a-3b4c-4d5e-8f0a-1b2c3d4e5f60");
    return iid;
}

/// @brief 测试接口（普通对象使用 CRefObject 的接口视图 Self<T>）。
class ITestRefObj : public virtual sc::IUnknown
{
public:
    virtual ~ITestRefObj() {}
    virtual int GetValue() const = 0;
};

/// @brief 普通对象（非模块）：继承 CRefObject 复用引用 / 弱引用能力。
class CTestRefObj : public sc::CRefObject, public ITestRefObj
{
public:
    CTestRefObj() : m_nValue(42) {}

    int GetValue() const override { return m_nValue; }

    void* QueryInterfaceImpl(const sc::InterfaceId& iid) override
    {
        if (iid == IID_ITestRefObj())
        {
            return static_cast<ITestRefObj*>(this);
        }
        return sc::CRefObject::QueryInterfaceImpl(iid);
    }

    int m_nValue;
};

} // namespace

/// @brief 引用计数与接口查询。
TEST(Module_RefCountAndQuery)
{
    sc::CModule* pModule = new CTestModule();
    ASSERT_EQ(pModule->AddRef(), 2u); // 创建时 1，AddRef 后 2
    ASSERT_EQ(pModule->Release(), 1u);

    void* ppv = pModule->QueryInterface(sc::IID_IUnknown());
    ASSERT_TRUE(ppv != nullptr);
    ASSERT_EQ(pModule->Release(), 0u); // 归零销毁
}

/// @brief 弱引用：模块存活时可升级为强引用，销毁后失效。
TEST(WeakRef_LockAndExpire)
{
    sc::CModule* pModule = new CTestModule("weak_live");
    sc::CWeakPtr<sc::IModule> wp = pModule->WeakSelf();

    // 存活时：Expired() 为 false，Lock() 返回有效强引用（计数 1→2）
    ASSERT_TRUE(!wp.Expired());
    sc::ScopedInterfacePtr<sc::IModule> spStrong = wp.Lock();
    ASSERT_TRUE(spStrong);

    // 释放初始引用（计数 2→1）：模块仍存活，弱引用仍有效
    pModule->Release();
    ASSERT_TRUE(!wp.Expired());
    ASSERT_TRUE(wp.Lock());

    // 释放强引用（计数 1→0）：模块销毁，弱引用失效
    spStrong.Reset();
    ASSERT_TRUE(wp.Expired());
    ASSERT_TRUE(!wp.Lock());
}

/// @brief 弱引用：模块销毁（直接归零）后 Lock 返回空。
TEST(WeakRef_ExpiredAfterDestroy)
{
    sc::CModule* pModule = new CTestModule("weak_dead");
    sc::CWeakPtr<sc::IModule> wp = pModule->WeakSelf();
    ASSERT_TRUE(!wp.Expired());

    pModule->Release(); // 计数 1→0，delete this
    ASSERT_TRUE(wp.Expired());
    ASSERT_TRUE(!wp.Lock());
}

/// @brief 弱引用：多线程并发 Lock / AddRef / Release 无竞争，Lock 成功后对象必然有效。
///
/// 注意：模块生命周期由单线程编排（CModuleManager），归零销毁只在最后
/// 由主线程单线程触发；各工作线程的 Lock/Release 在底引用保护下永不归零，
/// 避免制造"两个线程并发释放最后一个引用"的越界场景（无锁计数模型约定）。
TEST(WeakRef_ConcurrentLockAndRelease)
{
    sc::CModule* pModule = new CTestModule("weak_concurrent");
    sc::CWeakPtr<sc::IModule> wp = pModule->WeakSelf();

    // 主线程持有一个底强引用：保证各工作线程的 Lock/Release 永不触及归零销毁
    sc::ScopedInterfacePtr<sc::IModule> spMain = wp.Lock();
    ASSERT_TRUE(spMain);

    std::atomic<int> nLocks(0);
    std::atomic<int> nBadNames(0);
    std::vector<std::thread> vecThreads;
    for (int i = 0; i < 4; ++i)
    {
        vecThreads.push_back(std::thread([&wp, &nLocks, &nBadNames]()
        {
            for (int j = 0; j < 10000; ++j)
            {
                sc::ScopedInterfacePtr<sc::IModule> sp = wp.Lock();
                if (sp)
                {
                    ++nLocks;
                    if (sp->GetName() == nullptr)
                    {
                        ++nBadNames;
                    }
                }
            }
        }));
    }

    // 主线程释放初始引用（不归零：spMain 仍持有），与工作线程并发 Lock/Release 波动
    pModule->Release();
    for (size_t i = 0; i < vecThreads.size(); ++i)
    {
        vecThreads[i].join();
    }

    // 模块仍存活（spMain 持有），弱引用有效，Lock 期间对象必然有效
    ASSERT_TRUE(!wp.Expired());
    ASSERT_EQ(nBadNames.load(), 0); // Lock 成功后对象必然有效
    ASSERT_TRUE(nLocks.load() > 0);

    // 工作线程全部退出后，主线程释放最后强引用 → 唯一归零点（单线程销毁）
    spMain.Reset();
    ASSERT_TRUE(wp.Expired());
    ASSERT_TRUE(!wp.Lock());
}

/// @brief 普通对象（非模块）复用 CRefObject：强引用 / 弱引用 / 接口视图。
TEST(RefObject_GenericObject)
{
    CTestRefObj* pObj = new CTestRefObj;

    // 弱引用（IUnknown 视图）与强引用
    sc::CWeakPtr<sc::IUnknown> wp = pObj->WeakSelf();
    sc::ScopedInterfacePtr<sc::IUnknown> sp = pObj->Self();
    ASSERT_TRUE(sp);
    ASSERT_EQ(pObj->GetValue(), 42);

    // 模板接口视图：Self<ITestRefObj>()（RTTI 查找）
    sc::ScopedInterfacePtr<ITestRefObj> spI = pObj->Self<ITestRefObj>();
    ASSERT_TRUE(spI);
    ASSERT_EQ(spI->GetValue(), 42);

    // 计数：sp(2) + spI(3)；释放初始引用后仍存活
    pObj->Release();    // 3→2
    ASSERT_TRUE(!wp.Expired());

    spI.Reset();        // 2→1
    ASSERT_TRUE(!wp.Expired());
    sp.Reset();         // 1→0 → 销毁
    ASSERT_TRUE(wp.Expired());
    ASSERT_TRUE(!wp.Lock());
}

/// @brief 模块管理器生命周期编排。
TEST(ModuleManager_Lifecycle)
{
    sc::CModuleManager manager;
    CTestModule* pModule = new CTestModule();
    ASSERT_TRUE(manager.RegisterModule(IID_TestModule(), pModule)); // 接管

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
    ASSERT_TRUE(manager.RegisterModule(pModule)); // 按名字 "test" 注册（接管）

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
    ASSERT_TRUE(manager.RegisterModule(pByName)); // 按名字 "test"（接管）

    CTestModule* pByIid = new CTestModule();
    ASSERT_TRUE(manager.RegisterModule(IID_TestModule(), pByIid)); // 按接口（接管）

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
    ASSERT_TRUE(manager.RegisterModule(IID_TestModule(), pModule)); // 接管
    ASSERT_TRUE(manager.InitializeAll());
    ASSERT_TRUE(manager.StartAll());

    std::vector<sc::ModuleSnapshot> vecSnapshot = manager.Snapshot();
    ASSERT_EQ(vecSnapshot.size(), static_cast<size_t>(1));
    ASSERT_EQ(vecSnapshot[0].strIid, std::string("sc::TestModule"));
    ASSERT_EQ(vecSnapshot[0].strName, std::string("test"));
    ASSERT_EQ(vecSnapshot[0].state, sc::ModuleState::kStarted);
    ASSERT_EQ(vecSnapshot[0].strStatus, std::string("test-module"));
}

/// @brief 自持引用：Self() 返回指向自身的强引用，作用域结束引用释放。
///
/// 模拟回调场景：回调持有自持引用期间模块存活；引用对象销毁后引用归零。
TEST(Module_SelfReference)
{
    sc::CModule* pModule = new CTestModule("self-test");
    {
        sc::ScopedInterfacePtr<sc::IModule> spSelf = pModule->Self();
        ASSERT_TRUE(spSelf.Get() == pModule);
        // 引用计数 = 创建(1) + Self(1) = 2，作用域内模块必然存活
    }
    // spSelf 析构 → 引用计数回到 1，模块仍存活
    void* ppv = pModule->QueryInterface(sc::IID_IUnknown());
    ASSERT_TRUE(ppv != nullptr);
    ASSERT_EQ(pModule->Release(), 0u); // 归零销毁
}

/// @brief 消息路由器：结构体返回的提取器 + 粘包/跨包重组。
///
/// 协议：Length(4B 小端) + Type(4B) + Payload(Length 字节)。
TEST(MessageRouter_Dispatch)
{
    sc::CMessageRouter* pRouter = new sc::CMessageRouter();
    pRouter->SetExtractor(
        [](const char* pData, size_t nLen) -> sc::ExtractedMessage
        {
            sc::ExtractedMessage msg;
            msg.result = sc::MessageParseResult::kNeedMore;
            msg.step = 0;
            msg.type = 0;
            msg.payload = nullptr;
            msg.payloadSize = 0;
            if (nLen < 8)
            {
                return msg;
            }
            uint32_t nLength = 0;
            uint32_t nType = 0;
            for (int i = 0; i < 4; ++i)
            {
                nLength |= static_cast<uint32_t>(
                    static_cast<unsigned char>(pData[i])) << (8 * i);
                nType |= static_cast<uint32_t>(
                    static_cast<unsigned char>(pData[4 + i])) << (8 * i);
            }
            if (nLength > nLen - 8)
            {
                return msg; // 数据不足，等待更多
            }
            msg.result = sc::MessageParseResult::kOk;
            msg.step = 8 + nLength;
            msg.type = static_cast<int>(nType);
            msg.payload = pData + 8;
            msg.payloadSize = nLength;
            return msg;
        });

    std::vector<std::string> vecReceived;
    pRouter->RegisterHandler(
        1, [&vecReceived](sc::ConnectionId, int, const char* pPayload, size_t nLen)
        {
            vecReceived.push_back(std::string(pPayload, nLen));
        });

    // 构造两条消息（type=1, payload="hello"/"world"），每条 13 字节
    unsigned char buf[26];
    for (int i = 0; i < 13; ++i)
    {
        buf[i] = (i < 4) ? static_cast<unsigned char>((i == 0) ? 5 : 0) : 0;
    }
    buf[4] = 1;
    std::memcpy(buf + 8, "hello", 5);
    for (int i = 13; i < 26; ++i)
    {
        buf[i] = (i < 17) ? static_cast<unsigned char>((i == 13) ? 5 : 0) : 0;
    }
    buf[17] = 1;
    std::memcpy(buf + 21, "world", 5);

    // 第一次发 13 字节（消息1完整），第二次发 13 字节（消息2完整）
    pRouter->OnData(1, reinterpret_cast<const char*>(buf), 13);
    pRouter->OnData(1, reinterpret_cast<const char*>(buf + 13), 13);
    ASSERT_EQ(vecReceived.size(), static_cast<size_t>(2));
    ASSERT_EQ(vecReceived[0], std::string("hello"));
    ASSERT_EQ(vecReceived[1], std::string("world"));

    pRouter->OnClose(1);
    delete pRouter;
}
