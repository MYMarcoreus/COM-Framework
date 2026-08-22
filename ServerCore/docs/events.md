# 事件系统

模块间解耦通信：发布者与订阅者互不依赖，仅通过事件类型耦合。

## 接口（IEventDispatcher）

```cpp
class IEventDispatcher : public virtual IUnknown
{
    // 订阅事件，返回订阅标识
    virtual SubscriptionId Subscribe(const EventType& strType,
                                     const EventHandler& fnHandler) = 0;

    // 取消订阅
    virtual bool Unsubscribe(SubscriptionId nId) = 0;

    // 同步发布：在调用线程同步执行所有订阅者，返回处理器数量
    virtual size_t Publish(const EventType& strType,
                           const void* pData, size_t nSize) = 0;

    // 异步发布：将负载拷贝后投递到异步执行器线程处理，不阻塞调用线程
    virtual size_t PublishAsync(const EventType& strType,
                                const void* pData, size_t nSize) = 0;

    virtual size_t SubscriberCount(const EventType& strType) const = 0;
};
```

## 同步发布

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
            // ...
        }
    });
```

事件负载为**借用指针**（`Event.data`），仅在本次分发期间有效，订阅者不得长期持有。

## 异步发布（PublishAsync）

- `CEventDispatcher::Initialize(ctx)` 从注入上下文解析可选的 `IAsyncExecutor`；
- `PublishAsync` 拷贝负载 → 投递到执行器线程 → 在工作线程同步分发订阅者；
- 未配置异步执行器时**退化为同步发布**；
- 返回值为调用时快照的订阅者数量（参考值）。

### 启用异步

1. 在 `EventDispatcher` 之前注册 `CAsyncExecutorModule`（注册顺序保证先初始化）：
   ```cpp
   m_moduleManager.RegisterModule(sc::IID_IAsyncExecutor(), new sc::CAsyncExecutorModule(2));
   m_moduleManager.RegisterModule(sc::IID_IEventDispatcher(), new sc::CEventDispatcher());
   ```
2. 需要异步的场景调用 `PublishAsync`。

### 为什么用异步

`Publish` 是同步的，若订阅者耗时（写文件、批量重读配置），会**阻塞发布线程**
（可能是网络线程）。`PublishAsync` 把分发移到工作线程，保护发布线程实时性。

## 内置事件

`sc::events` 命名空间（`Event/EventTypes.h`）：

| 常量 | 触发方 | 负载 |
|---|---|---|
| `kNetworkStarted` | `CTcpServerModule` | 监听端口（uint16_t） |
| `kNetworkStopped` | `CTcpServerModule` | 无 |
| `kConfigReloaded` | `CConfigReloadModule` | 无 |

自定义事件直接使用字符串类型，如 `"demo.hello"`（进程内唯一）。

## 线程安全

`Subscribe` / `Unsubscribe` / `Publish` / `PublishAsync` 均可跨线程调用；
处理器在锁外执行，避免处理器内再次发布造成死锁。

## 参考实现

- `Demo/Application/DemoApplication.cpp`：订阅 network 事件 + `PublishAsync("demo.hello")` 示范
- `LogServer/Application/LogServerApplication.cpp`：订阅 network + config.reloaded 事件
