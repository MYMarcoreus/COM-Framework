# 网络层 — 实现文档

> 配套使用文档：[network-usage.md](network-usage.md)
> 源码：`ServerCore/Network/*` + `Common/Network/*`

## 1. CTcpServer（Common::network，基于 asio）

**组件**：`asio::io_context m_io` + `unique_ptr<asio::ip::tcp::acceptor> m_pAcceptor`（**无 strand**）；
事件循环运行于独立 `std::thread m_thread`（`m_io.run()`），所有回调与共享状态仅在 io 线程访问；
跨线程操作用 `asio::post(m_io, ...)` 投递。

- **Start**：acceptor `open → reuse_address → bind → listen`，建线程；`Stop`：置 `m_bRunning(false)` → `post(ShutdownOnIoThread)` → `join`；
- **接收**：`async_accept` 回调中：`operation_aborted` 直接返回、瞬时错误重投、**连接数上限**达标直接关闭新 socket、
  `m_nNextId++` 生成 `ConnectionId`、构造 `CTcpConnection`（`shared_ptr` + `enable_shared_from_this`）、
  `StartRead`、记录 peer、更新 `m_nConnectionCount/m_nTotalAccepted`、触发 `m_fnAccept`；
- **回调线程**：`OnAccept/OnData/OnClose` 均在 io 线程同步执行；`HandleData` 把 `CBuffer` 数据直接转发 `m_fnData`；
- **发送缓冲**：`Send` 拷入 `std::string` 后 `post` 到 io 线程；`CTcpConnection` 内 `m_strPendingOutput` 累积 +
  `m_bWriting` 标志防重入（`AppendWrite/DoWrite/HandleWrite`）；
- **空闲超时**：`SetIdleTimeout` → 每秒 `steady_timer::async_wait`，收集超时连接调 `Close`（投递 io 线程）；
- **统计**：`ConnectionCount/TotalAccepted/TotalClosed` 为 `atomic` 直读。

## 2. CNetworkModule（实现 INetwork）

- 持有 `unique_ptr<CTcpServer> m_pServer`、`ScopedInterfacePtr<INetworkHandler> m_pHandler`、可选 `IMetrics`；
- `StartTcpServer`：先 `Stop` 旧实例，把 `INetworkHandler` 的 `OnAccept/OnData/OnClose` 适配为 CTcpServer 回调；
  回调中**先更新指标再转发**：accept → `Inc("network.accepted")` + `SetGauge("network.conns", ConnectionCount())`；
  data → `Inc("network.msgs")`；close → `Inc("network.closed")` + 更新 conns；
- `Stop`：锁内 `release` 服务器指针，再在**锁外** `Stop` + `delete`（避免等待事件循环线程时死锁）；
- 连接级上下文：`std::map<ConnectionId,void*> m_mapConnCtx` + `m_mutex`，`Attach/GetAttached/Detach` 均持锁；
- `Send/Close/GetStatus` 在 `m_mutex` 保护下转发给 server。

## 3. CTcpServerModule（装配模块）

- 构造 `AddDependency(IID_INetwork)` + `IID_INetworkHandler)` 保证顺序；`Initialize` 解析 `INetwork`、
  `INetworkHandler`、可选 `IEventDispatcher`；
- `Start`：`m_pNetwork->StartTcpServer(port, handler)` 成功后 `Publish(kNetworkStarted, &port, ...)`；
- `Stop`：先 `Publish(kNetworkStopped, ...)` 再 `m_pNetwork->Stop()`。

## 4. 线程模型小结

| 线程 | 访问内容 |
|---|---|
| io 事件循环线程 | acceptor、连接 map、peer 地址、发送缓冲、回调执行 |
| 业务工作线程 | 经 `INetworkHandler` 转发的业务处理、`Send/Close`（经 `post` 投递 io 线程） |
| 任意线程 | `ConnectionCount/TotalAccepted/TotalClosed`（原子直读）、`Attach/GetAttached/Detach`（持锁） |
