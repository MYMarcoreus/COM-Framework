# 事件系统 — 使用文档

> 实现细节见：[events-impl.md](events-impl.md)

## 1. 接口（IEventDispatcher）

```cpp
class IEventDispatcher : public virtual IUnknown
{
    virtual SubscriptionId Subscribe(const EventType& strType,
                                     const EventHandler& fnHandler) = 0;  // 返回订阅标识
    virtual bool Unsubscribe(SubscriptionId nId) = 0;
    virtual size_t Publish(const EventType& strType,
                           const void* pData, size_t nSize) = 0;   // 同步分发
    virtual size_t PublishAsync(const EventType& strType,
                                const void* pData, size_t nSize) = 0; // 异步分发
    virtual size_t SubscriberCount(const EventType& strType) const = 0;
};
```

## 2. 同步发布

```cpp
// 发布端
m_pEventDispatcher->Publish(sc::events::kNetworkStarted, &port, sizeof(port));
// 订阅端
m_pEventDispatcher->Subscribe(sc::events::kNetworkStarted,
    [](const sc::Event& ev)
    {
        if (ev.data != nullptr && ev.size == sizeof(std::uint16_t))
        {
            std::uint16_t port = *static_cast<const std::uint16_t*>(ev.data);
        }
    });
```

事件负载为**借用指针**（`Event.data`），仅在本次分发期间有效，订阅者不得长期持有。

## 3. 异步发布（PublishAsync）

- `CEventDispatcher::Initialize(ctx)` 从注入上下文解析可选的 `IAsyncExecutor`；
- `PublishAsync` 拷贝负载 → 投递到执行器线程 → 在工作线程同步分发订阅者；
- 未配置异步执行器时**退化为同步发布**。

### 启用异步

```cpp
m_moduleManager.RegisterModule(sc::IID_IAsyncExecutor(), new sc::CAsyncExecutorModule(2));
m_moduleManager.RegisterModule(sc::IID_IEventDispatcher(), new sc::CEventDispatcher());
```

### 为什么用异步

`Publish` 是同步的，若订阅者耗时（写文件、批量重读配置）会**阻塞发布线程**（可能是网络线程）；
`PublishAsync` 把分发移到工作线程，保护发布线程实时性。

## 4. 内置事件

`sc::events` 命名空间（`Event/EventTypes.h`）：`kNetworkStarted`（端口）、`kNetworkStopped`、`kConfigReloaded`。
自定义事件直接使用字符串类型，如 `"demo.hello"`（进程内唯一）。

## 5. 线程安全

`Subscribe / Unsubscribe / Publish / PublishAsync` 均可跨线程调用；处理器在**锁外**执行，避免处理器内再次发布造成死锁。

## 6. 参考实现

- `Demo/Application/DemoApplication.cpp`：订阅 network 事件 + `PublishAsync("demo.hello")` 示范
- `LogServer/Application/LogServerApplication.cpp`：订阅 network + config.reloaded 事件
