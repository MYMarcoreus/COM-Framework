# 模块模型

ServerCore 的模块体系基于 COM 思想：接口 + 引用计数 + 接口查询 + 统一生命周期。

## 核心类型

| 类型 | 职责 |
|---|---|
| `IUnknown` | 根接口：`QueryInterface` / `AddRef` / `Release` |
| `InterfaceId` | 128 位 GUID 接口标识，`IID_XXX()` 常量 |
| `ScopedInterfacePtr<T>` | RAII 接口指针（自动 AddRef / Release） |
| `CWeakPtr<T>` | 弱引用（不延长生命周期，`Lock()` 升级为强引用） |
| `CRefObject` | 引用计数基础类：`AddRef` / `Release` / 接口查询 / `Self` / `WeakSelf` |
| `IModule` | 模块接口：名称 / 状态 / 生命周期 / 依赖 / 状态报告 |
| `CModule` | 模块基类：继承 `CRefObject`，模块生命周期 / 状态 / 依赖 / 状态报告 |
| `CModuleManager` | 统一管理器：注册、拓扑排序、生命周期编排、快照 |

## 生命周期状态

```text
kCreated → kInitialized → kStarted → kStopped → kShutdown
```

由 `CModuleManager` 统一驱动，外部不可直接修改（`friend` 访问 `SetState`）。

## 定义一个新模块

```cpp
/// @brief 业务模块：通过接口暴露能力，生命周期由管理器编排。
class CMyModule : public sc::CModule, public sc::IMyInterface
{
public:
    CMyModule() : sc::CModule("my-module")
    {
        // 声明硬依赖：保证被依赖模块先初始化 / 启动
        AddDependency(sc::IID_IConfig());
    }

    // 依赖通过注入的上下文解析
    bool Initialize(const sc::CResolveContext& ctx) override
    {
        m_pConfig.Reset(ctx.Resolve<sc::IConfig>());
        return m_pConfig != nullptr;
    }

    bool Start() override { /* 开始工作 */ return true; }
    void Stop() override  { /* 停止工作 */ }
    void Shutdown() override { m_pConfig.Reset(); }

    // 暴露接口
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

## 注册与生命周期编排

```cpp
CModuleManager manager;
manager.RegisterModule(iid, new CMyModule());          // 接管型：不额外 AddRef
manager.InitializeAll();   // 按依赖拓扑初始化，传 CResolveContext
manager.StartAll();        // 按依赖拓扑启动
manager.StopAll();         // 逆序停止
manager.ShutdownAll();     // 逆序关闭
manager.Clear();           // 释放全部
```

## 服务定位

- 按接口标识注册：`RegisterModule(IID_INetwork(), new CNetworkModule())`
- 解析：`ctx.Resolve<sc::INetwork>()`（类型自动绑定）或 `ctx.Resolve<T>(iid)`（显式）
- 同一接口支持多实例：`GetModulesByIid(iid)` / `ModuleCountByIid(iid)`

## 引用计数基础类（CRefObject）

引用计数、接口查询、强引用 / 弱引用能力由 `CRefObject` 统一提供，`CModule` 继承它。
**任何需要引用 / 弱引用的对象**（模块、连接、业务上下文等）都可继承 `CRefObject` 复用：

```cpp
// 非模块对象示例
class CConnection : public sc::CRefObject, public IConnection
{
public:
    // 重写接口查询以暴露自身接口
    void* QueryInterfaceImpl(const sc::InterfaceId& iid) override { /* ... */ }
};

sc::CWeakPtr<sc::IConnection> spWeak = pConn->WeakSelf<sc::IConnection>();
sc::ScopedInterfacePtr<sc::IConnection> sp = pConn->Self<sc::IConnection>();
```

- `Self()`（等价 `Self<IUnknown>()`）：自持强引用；`Self<T>()` 返回指定接口视图（RTTI 查找）。
- `WeakSelf()` / `WeakSelf<T>()`：对应弱引用，不延长生命周期。
- `CModule` 提供便捷的 `Self()` / `WeakSelf()`（返回 `IModule` 视图），业务模块无需关心模板参数。

## 自持引用（回调安全）

回调 / 异步任务中必须保证模块存活，使用 `Self()`。推荐用**具体类型**的自持引用，
回调只捕获 `spSelf`（通过 `spSelf->` 调用成员），无需再捕获裸 `this`：

```cpp
// 具体类型自持引用：生命周期由 spSelf 保证，访问入口由具体类型提供
sc::ScopedInterfacePtr<CDemoService> spSelf = Self<CDemoService>();
m_pTimer->AddPeriodicTimer(interval, [spSelf]()
{
    if (spSelf) { spSelf->DoSomething(); /* 模块必然存活 */ }
});
```

也可以使用 `IModule` 视图（此时回调里需要额外捕获 `this` 才能调用具体成员）：

```cpp
sc::ScopedInterfacePtr<sc::IModule> spSelf = Self();
m_pTimer->AddPeriodicTimer(interval, [this, spSelf]()
{
    if (spSelf) { /* 模块必然存活 */ }
});
```

## 弱引用（不延长生命周期）

`Self()` 强引用会延长模块生命周期：任务排队期间模块被持有，模块 `Shutdown` 后回调仍会执行（僵尸回调）。若希望"模块已关闭则丢弃结果"，使用弱引用：

```cpp
sc::CWeakPtr<sc::IModule> spWeak = WeakSelf();
m_pExecutor->Post([spWeak]()
{
    sc::ScopedInterfacePtr<sc::IModule> spStrong = spWeak.Lock();
    if (!spStrong) { return; }   // 模块已销毁，丢弃结果
    /* 安全使用 spStrong */
});
```

- `CWeakPtr<T>`：不持有模块，模块析构后自动失效（`Expired()`），不会阻止销毁。
- `Lock()`：升级为强引用；模块存活返回有效 `ScopedInterfacePtr<T>`，已销毁返回空。
- 线程安全：`Lock()` 与模块归零销毁在同一把锁内互斥（见 `CModuleLifetime`），**绝不访问已销毁的内存**，可安全用于多线程并发回调。
- **并发约定**：模块生命周期由 `CModuleManager` 单线程编排；业务通过 `Self()` / `Lock()` 持有的强引用应在模块 `Shutdown` 前或执行器 `Stop`（等待任务完成）前释放。`CWeakPtr::Lock()` 与归零销毁互斥、线程安全；但普通引用计数的归零销毁遵循 COM 无锁计数模型约定——**禁止两个线程并发释放同一模块的最后一个引用**（归零销毁应发生在单线程编排的关闭阶段）。

选择准则：

| 场景 | 推荐 |
|---|---|
| 回调必须执行（结果要落地，如发响应/写盘） | `Self()` 强引用 + 状态检查 |
| 结果通知 / 允许丢弃（模块关了就不要了） | `CWeakPtr` 弱引用 |
| 任务可能积压，不想长占模块 | `CWeakPtr` 弱引用 |

## 状态查询与报告

- `GetModuleState(name)` / `HasModule(name)` / `Snapshot()`：健康检查 / 日志
- `CModule::GetStatus()`：模块自述状态文本（如 `network:port=9000 conns=2`）
