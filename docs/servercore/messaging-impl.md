# 消息流水线 — 实现文档

> 配套使用文档：[messaging-usage.md](messaging-usage.md)
> 源码：`ServerCore/Message/MessageRouter.*`

## 1. 数据结构

```cpp
std::map<ConnectionId, std::string> m_mapBuffers;                    // 按连接维护接收缓冲
std::map<SubscriptionId, HandlerEntry> m_mapHandlers;                // ID → {type, handler}
std::map<int, std::vector<SubscriptionId>> m_mapByType;              // 类型 → ID 列表
std::function<ExtractedMessage(const char*, size_t)> m_fnExtractor;  // 提取器
mutable std::mutex m_mutex;   // 统一保护
size_t m_nNextId;             // 订阅 ID 自增
```

## 2. OnData：缓冲 + 循环切分

```cpp
// ① 锁内取出该连接旧缓冲（erase 后再 append，避免锁外共享）
std::string strPending; { lock; it = m_mapBuffers.find(nId); if (it!=end) { strPending = it->second; erase; } }
strPending.append(pData, nLen);

// ② 锁外循环调用提取器切分
size_t nConsumed = 0;
while (true)
{
    ExtractedMessage m = m_fnExtractor(strPending.data()+nConsumed, strPending.size()-nConsumed);
    if (m.result == kNeedMore) break;             // 半包：等待更多
    if (m.result == kInvalid || m.step == 0) { nConsumed = strPending.size(); break; } // 丢弃剩余，防死循环
    Dispatch(nId, m.type, m.payload, m.payloadSize);  // 按类型分发
    nConsumed += m.step;
}

// ③ 剩余 substr(nConsumed) 锁内放回或 erase
```

- 同连接数据由网络层保证**串行进入**；
- `kInvalid` 或 `step==0` 时丢弃该连接全部剩余数据（防呆，防死循环）；
- 缓冲无上限（业务协议层负责限制最大包长）。

## 3. SetExtractor / RegisterHandler / UnregisterHandler

- `SetExtractor`：锁内赋值 `m_fnExtractor`；
- `RegisterHandler`：校验非空 → 锁内递增 ID、写两 map；
- `UnregisterHandler`：与事件侧 `Unsubscribe` 同构（按 type 向量线性删除）。

## 4. Dispatch（锁外调用处理器）

```cpp
std::vector<MessageHandler> vecHandlers;
{ lock; for (id : m_mapByType[type]) vecHandlers.push_back(handler); }
for (h : vecHandlers) h(nId, type, payload, payloadSize);  // 锁外，防处理器内重入本模块死锁
```

## 5. OnClose 与线程安全

- `OnClose`：锁内 `m_mapBuffers.erase(nId)` 清理该连接缓冲；
- `OnData / OnClose` 与 `SetExtractor / RegisterHandler` 可跨线程安全调用；
- 处理器**锁外**调用（与事件系统同款「快照 + 锁外」防重入手法）。

## 6. 与协议、序列化的组合

提取器切出负载 → 负载内用 `CBinaryReader` 解字段（见 [serialization-usage.md](../common/serialization-usage.md)）。
