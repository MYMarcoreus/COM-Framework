# 依赖注入 — 使用文档

> 实现细节见：[dependency-injection-impl.md](dependency-injection-impl.md)

## 1. 动机

模块不持有 `CModuleManager&`（装配器全局引用），而是把依赖作为 `Initialize(ctx)` 的参数注入：

- **显式**：模块声明依赖什么一目了然，便于单测（可注入 mock 接口模块）；
- **类型安全**：`ctx.Resolve<T>()` 自动绑定「类型 ↔ 接口标识」，少一个 iid 与类型不匹配的错误源；
- **低耦合**：模块不依赖装配器，可独立编译 / 测试。

## 2. CResolveContext

```cpp
class CResolveContext
{
public:
    explicit CResolveContext(CModuleManager& manager);

    template <typename T> T* Resolve() const;          // 按类型自动绑定解析（需 InterfaceIdOf<T> 特化）
    template <typename T> T* Resolve(const InterfaceId& iid) const; // 显式指定接口标识

private:
    CModuleManager& m_manager;
};
```

## 3. InterfaceIdOf<T>（类型 → 接口标识）

特化映射表覆盖 ServerCore 全部内置接口：`IConfig / ILogger / ITimer / IThreadPool / IAsyncExecutor /
INetwork / INetworkHandler / IEventDispatcher / IMessageRouter / IMetrics`。

## 4. 解析依赖

```cpp
// 硬依赖（必须存在，缺失即失败）
bool CMyModule::Initialize(const sc::CResolveContext& ctx)
{
    m_pNetwork.Reset(ctx.Resolve<sc::INetwork>());
    if (m_pNetwork == nullptr) return false;
    return true;
}

// 可选依赖（缺失不失败）
m_pMetrics.Reset(ctx.Resolve<sc::IMetrics>());

// 自定义接口（无 InterfaceIdOf 特化，用显式 iid 版）
m_pMySvc.Reset(ctx.Resolve<sc::IMyInterface>(sc::IID_IMyInterface()));
```

## 5. 生命周期保证

- `InitializeAll()` 构造 `CResolveContext(*this)` 传入每个模块；
- 依赖模块通过 `AddDependency(iid)` 保证先初始化 / 启动（拓扑排序）；
- 可选依赖不 `AddDependency`，靠**注册顺序**保证：先注册者先初始化 / 启动。

## 6. 单测技巧

```cpp
sc::CModuleManager manager;
manager.RegisterModule(sc::IID_IConfig(), new sc::CConfigModule()); // mock 或真实
manager.RegisterModule(sc::IID_INetwork(), new MockNetworkModule());

sc::CResolveContext ctx(manager);
CMyModule module;
ASSERT_TRUE(module.Initialize(ctx));
```
