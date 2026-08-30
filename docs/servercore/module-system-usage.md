# 模块系统 — 使用文档

> 实现细节见：[module-system-impl.md](module-system-impl.md)

## 1. 核心类型

ServerCore 的模块体系基于 COM 思想：接口 + 引用计数 + 接口查询 + 统一生命周期。

| 类型 | 职责 |
|---|---|
| `IUnknown` | 根接口：`QueryInterface` / `AddRef` / `Release` |
| `InterfaceId` | 128 位 GUID 接口标识，`IID_XXX()` 常量 |
| `ScopedInterfacePtr<T>` | RAII 接口指针（自动 AddRef / Release） |
| `CWeakPtr<T>` | 弱引用（不延长生命周期，`Lock()` 升级为强引用） |
| `CRefObject` | 引用计数基础类：`AddRef` / `Release` / 接口查询 / `Self` / `WeakSelf` |
| `IModule` | 模块接口：名称 / 状态 / 生命周期 / 依赖 / 状态报告 |
| `CModule` | 模块基类：继承 `CRefObject`，生命周期 / 状态 / 依赖 / 状态报告 |
| `CModuleManager` | 统一管理器：注册、拓扑排序、生命周期编排、快照 |

## 2. 生命周期状态

```text
kCreated → kInitialized → kStarted → kStopped → kShutdown
```

由 `CModuleManager` 统一驱动，外部不可直接修改。

## 3. 定义一个新模块

```cpp
/// @brief 业务模块：通过接口暴露能力，生命周期由管理器编排。
class CMyModule : public sc::CModule, public sc::IMyInterface
{
public:
    CMyModule() : sc::CModule("my-module")
    {
        AddDependency(sc::IID_IConfig()); // 硬依赖：保证先初始化/启动
    }

    bool Initialize(const sc::CResolveContext& ctx) override
    {
        m_pConfig.Reset(ctx.Resolve<sc::IConfig>());
        return m_pConfig != nullptr;
    }

    bool Start() override { /* 开始工作 */ return true; }
    void Stop() override  { /* 停止工作 */ }
    void Shutdown() override { m_pConfig.Reset(); }

    void* QueryInterfaceImpl(const sc::InterfaceId& iid) override
    {
        if (iid == sc::IID_IMyInterface())
        {
            return static_cast<sc::IMyInterface*>(this);
        }
        return sc::CModule::QueryInterfaceImpl(iid);
    }

private:
    sc::ScopedInterfacePtr<sc::IConfig> m_pConfig;
};
```

## 4. 注册与生命周期编排

```cpp
CModuleManager manager;
manager.RegisterModule(iid, new CMyModule());   // 接管型：不额外 AddRef
manager.InitializeAll();   // 按依赖拓扑初始化，传 CResolveContext
manager.StartAll();        // 按依赖拓扑启动
manager.StopAll();         // 逆序停止
manager.ShutdownAll();     // 逆序关闭
manager.Clear();           // 释放全部
```

## 5. 服务定位

- 按接口标识注册：`RegisterModule(IID_INetwork(), new CNetworkModule())`
- 解析：`ctx.Resolve<sc::INetwork>()`（类型自动绑定）或 `ctx.Resolve<T>(iid)`（显式）
- 同一接口支持多实例：`GetModulesByIid(iid)` / `ModuleCountByIid(iid)`

## 6. 引用计数基础类（CRefObject）

**任何需要引用 / 弱引用的对象**（模块、连接、业务上下文等）都可继承 `CRefObject` 复用：

```cpp
class CConnection : public sc::CRefObject, public IConnection
{
    void* QueryInterfaceImpl(const sc::InterfaceId& iid) override { /* ... */ }
};

sc::CWeakPtr<sc::IConnection> spWeak = pConn->WeakSelf<sc::IConnection>();
sc::ScopedInterfacePtr<sc::IConnection> sp = pConn->Self<sc::IConnection>();
```

- `Self()`（等价 `Self<IUnknown>()`）：自持强引用；`Self<T>()` 返回指定接口视图（RTTI 查找）；
- `WeakSelf()` / `WeakSelf<T>()`：对应弱引用，不延长生命周期；
- `CModule` 提供便捷的 `Self()` / `WeakSelf()`（返回 `IModule` 视图）。

## 7. 自持引用（回调安全）

回调 / 异步任务中必须保证模块存活，使用 `Self()`。推荐**具体类型**自持引用：

```cpp
sc::ScopedInterfacePtr<CDemoService> spSelf = Self<CDemoService>();
m_pTimer->AddPeriodicTimer(interval, [spSelf]()
{
    if (spSelf) { spSelf->DoSomething(); /* 模块必然存活 */ }
});
```

## 8. 弱引用（不延长生命周期）

`Self()` 会延长模块生命周期（僵尸回调）；希望「模块已关闭则丢弃结果」用弱引用：

```cpp
sc::CWeakPtr<sc::IModule> spWeak = WeakSelf();
m_pExecutor->Post([spWeak]()
{
    sc::ScopedInterfacePtr<sc::IModule> spStrong = spWeak.Lock();
    if (!spStrong) { return; }   // 模块已销毁，丢弃结果
});
```

选择准则：

| 场景 | 推荐 |
|---|---|
| 一次性任务必须执行（发响应 / 写盘） | `Self()` 强引用 + 状态检查 |
| 周期定时任务（模块活着才执行） | `CWeakPtr` 弱引用 + `Lock()` |
| 结果通知 / 允许丢弃 | `CWeakPtr` 弱引用 |

周期定时任务推荐守卫模板 `sc::AddGuardedPeriodicTimer`（`Infra/GuardedTimer.h`）：

```cpp
m_tTimerId = sc::AddGuardedPeriodicTimer(m_pTimer.Get(), m_nIntervalMs,
    WeakSelf<CDemoXxx>(), [](const sc::ScopedInterfacePtr<CDemoXxx>& sp) {
        sp->DoSomething();
    });
```

## 9. 状态查询与报告

- `GetModuleState(name)` / `HasModule(name)` / `Snapshot()`：健康检查 / 日志
- `CModule::GetStatus()`：模块自述状态文本（如 `network:port=9000 conns=2`）
