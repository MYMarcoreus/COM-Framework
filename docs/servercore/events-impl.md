# 事件系统 — 实现文档

> 配套使用文档：[events-usage.md](events-usage.md)
> 源码：`ServerCore/Event/EventDispatcher.*`

## 1. 数据结构（双 map 索引）

```cpp
std::map<SubscriptionId, Subscription> m_mapSubscriptions;   // ID → {type, handler}
std::map<EventType, std::vector<SubscriptionId>> m_mapByType; // 类型 → ID 列表
mutable std::mutex m_mutex;   // 统一保护
size_t m_nNextId;             // 订阅 ID 自增生成（初始 1）
```

双索引：订阅按 ID 精确查（取消），按类型快速查（发布）；同一类型多订阅按 `vector` 顺序保存。

## 2. Subscribe / Unsubscribe

- `Subscribe`：校验 handler 非空（空返回 `kInvalidSubscriptionId=0`），锁内 `m_nNextId++` 生成 ID，同时写两个 map；
- `Unsubscribe`：锁内按 ID 查 `m_mapSubscriptions`，从 `m_mapByType[type]` 向量中线性删除该 ID（删空则 erase 整个 type 条目），
  再删订阅条目；无效 ID 返回 false。

## 3. Publish（同步）

```cpp
// 锁内快照出 handler 向量 → 释放锁 → 锁外依次调用
std::vector<EventHandler> vecTargets;
{ lock; for (id : m_mapByType[type]) vecTargets.push_back(handler); }
for (h : vecTargets) h(ev);   // 锁外调用，防处理器内再 Publish 递归持锁死锁
return vecTargets.size();
```

- 返回值为实际分发到的处理器数量；无订阅返回 0；
- 负载以借用指针传入，仅分发期间有效。

## 4. PublishAsync（异步分发）

1. 锁内快照订阅者数 `nCount`；
2. `m_pExecutor == nullptr` → **退化同步** `return Publish(...)`；
3. 锁外**拷贝负载**到 `std::vector<char>`（借用指针异步后失效）；
4. `m_pExecutor->Post([...])`：lambda 捕获 `Self<CEventDispatcher>()` **自持引用**保证模块存活，
   入队后调用同步 `Publish`；
5. 返回 `bPosted ? nCount : 0`（投递成功返回快照参考值，失败 0）。

## 5. SubscriberCount 与线程安全

- `SubscriberCount` 锁内返回 `m_mapByType[type].size()`；
- 所有操作锁内、处理器**锁外**调用，可跨线程安全调用。

## 6. 与异步执行器的关系

`Initialize(ctx)` 用 `ctx.Resolve<IAsyncExecutor>()` 解析可选执行器（缺失不算失败），存
`ScopedInterfacePtr<IAsyncExecutor> m_pExecutor`；`PublishAsync` 调其 `Post(fn)` 提交无返回值任务
（该接口适配 `common::async::CAsyncExecutor`）。

**核心手法**：订阅表与处理器执行分离 + 「快照 + 锁外调用」防重入死锁；
异步路径以「拷贝负载 + 自持引用」保证借用指针安全。
