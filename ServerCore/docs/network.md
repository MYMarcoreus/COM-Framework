# 网络层

## 组件

| 组件 | 职责 |
|---|---|
| `INetwork` | 网络接口：启动 / 停止 / 发送 / 关闭 / 统计 / 连接级上下文 |
| `INetworkHandler` | 网络事件回调：`OnAccept` / `OnData` / `OnClose` |
| `CNetworkModule` | 实现 `INetwork`，内部封装 `common::CTcpServer`（asio） |
| `CTcpServerModule` | 通用 TCP 装配模块：解析网络 / 处理 / 事件接口并启动 |

## 数据流

```text
CTcpClient / 其他客户端
        ↓ TCP
CNetworkModule（CTcpServer）
        ↓ INetworkHandler 回调
业务 Service（INetworkHandler）
        ↓ 消息流水线
业务命令处理
```

## 连接级上下文

业务可为每个连接挂载任意上下文对象，无需自己维护 `map<ConnectionId,...>`：

```cpp
// INetwork 新增接口
virtual void* Attach(ConnectionId nId, void* pCtx);   // 挂载，返回旧值
virtual void* GetAttached(ConnectionId nId) const;    // 取回
virtual void* Detach(ConnectionId nId);               // 移除并返回（所有权交还）
```

**所有权归业务**：框架仅存储引用，不负责释放；业务应在 `OnClose` 中
`Detach` 并清理。参考实现：`Demo/Service/DemoService.cpp`（`ConnContext`）。

```cpp
void CMyService::OnAccept(sc::ConnectionId id, const std::string& peer)
{
    m_pNetwork->Attach(id, new ConnContext(peer));   // 连接建立时挂载
}
void CMyService::OnClose(sc::ConnectionId id)
{
    ConnContext* pCtx = static_cast<ConnContext*>(m_pNetwork->Detach(id));
    if (pCtx != nullptr) { /* 使用并释放 */ delete pCtx; }
}
```

## 回调线程

- `OnAccept` / `OnData` / `OnClose` 在 asio 事件循环线程执行；
- **回调应尽快返回**：重活应通过 `IAsyncExecutor` 投递到工作线程
  （参考 `CDemoService::HandlePing` / `HandleEcho`），避免阻塞网络线程；
- 异步任务必须捕获模块自持引用（`Self()`）保证回调期间模块存活。

## 统计与限制

- `ConnectionCount` / `TotalAccepted` / `TotalClosed` / `PeerAddress`；
- `SetIdleTimeout(seconds)`：空闲超时自动关闭；
- `SetMaxConnections(n)`：超限拒绝新连接。

## 装配

`CNetworkModule` 按 `IID_INetwork()` 注册；`CTcpServerModule` 从注入上下文解析
`INetwork` / `INetworkHandler` / `IEventDispatcher`，`Start()` 时启动监听并发布
`network.started` 事件，`Stop()` 发布 `network.stopped`。
