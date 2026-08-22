# 依赖注入（CResolveContext）

## 动机

模块不再持有 `CModuleManager&`（装配器全局引用），而是把依赖作为
`Initialize(ctx)` 的参数注入。好处：

- **显式**：模块声明依赖什么一目了然，便于单测（可注入 mock 接口模块）；
- **类型安全**：`ctx.Resolve<T>()` 自动绑定"类型 ↔ 接口标识"，少一个 iid 与类型不匹配的错误源；
- **低耦合**：模块不依赖装配器，可独立编译 / 测试。

## 核心类型

### `CResolveContext`（`Module/ResolveContext.h`）

```cpp
class CResolveContext
{
public:
    explicit CResolveContext(CModuleManager& manager);

    // 按类型自动绑定接口标识解析（需 InterfaceIdOf<T> 特化）
    template <typename T> T* Resolve() const;

    // 显式指定接口标识解析（自定义接口使用）
    template <typename T> T* Resolve(const InterfaceId& iid) const;

private:
    CModuleManager& m_manager;
};
```

### `InterfaceIdOf<T>`

类型 → 接口标识 的特化映射表，覆盖 ServerCore 全部内置接口：

| 类型 | 接口标识 |
|---|---|
| `IConfig` | `IID_IConfig()` |
| `ILogger` | `IID_ILogger()` |
| `ITimer` | `IID_ITimer()` |
| `IThreadPool` | `IID_IThreadPool()` |
| `IAsyncExecutor` | `IID_IAsyncExecutor()` |
| `INetwork` | `IID_INetwork()` |
| `INetworkHandler` | `IID_INetworkHandler()` |
| `IEventDispatcher` | `IID_IEventDispatcher()` |
| `IMessageRouter` | `IID_IMessageRouter()` |
| `IMetrics` | `IID_IMetrics()` |

## 使用

### 解析硬依赖（必须存在）

```cpp
bool CMyModule::Initialize(const sc::CResolveContext& ctx)
{
    m_pNetwork.Reset(ctx.Resolve<sc::INetwork>());
    if (m_pNetwork == nullptr) return false;   // 依赖缺失即失败
    return true;
}
```

### 解析可选依赖（缺失不失败）

```cpp
m_pMetrics.Reset(ctx.Resolve<sc::IMetrics>()); // 缺失时为空，仅不上报
```

### 自定义接口

业务自定义接口没有 `InterfaceIdOf` 特化，用显式 iid 版本：

```cpp
m_pMySvc.Reset(ctx.Resolve<sc::IMyInterface>(sc::IID_IMyInterface()));
```

## 生命周期保证

- `InitializeAll()` 构造 `CResolveContext(*this)` 传入每个模块；
- 依赖模块通过 `AddDependency(iid)` 保证先初始化 / 启动（拓扑排序）；
- 可选依赖不 `AddDependency`，靠**注册顺序**保证：先注册者先初始化 / 启动。

## 单测技巧

```cpp
sc::CModuleManager manager;
manager.RegisterModule(sc::IID_IConfig(), new sc::CConfigModule()); // mock 或真实
manager.RegisterModule(sc::IID_INetwork(), new MockNetworkModule());

sc::CResolveContext ctx(manager);
CMyModule module;
ASSERT_TRUE(module.Initialize(ctx));
```
