# 消息流水线 — 使用文档

> 实现细节见：[messaging-impl.md](messaging-impl.md)

## 1. 接口（IMessageRouter）

协议无关的消息流水线：**缓冲 / 半包 / 粘包切分 / 按类型分发**全部下沉到框架，业务只提供「提取器」与「命令处理器」。

```cpp
class IMessageRouter : public virtual IUnknown
{
    virtual void SetExtractor(const MessageExtractor& fnExtractor) = 0;
    virtual SubscriptionId RegisterHandler(int nType, const MessageHandler& fnHandler) = 0;
    virtual bool UnregisterHandler(SubscriptionId nId) = 0;
    virtual void OnData(ConnectionId nId, const char* pData, size_t nLen) = 0;
    virtual void OnClose(ConnectionId nId) = 0;
};
```

## 2. 提取器（MessageExtractor）

```cpp
using MessageExtractor =
    std::function<ExtractedMessage(const char* data, size_t len)>;
```

业务根据协议从流起始处提取一条完整消息，返回：

| 字段 | 含义 |
|---|---|
| `result` | `kNeedMore`（半包）/ `kOk`（完整）/ `kInvalid`（非法） |
| `step` | 本条消息消耗的字节数（kOk 时） |
| `type` | 消息类型（kOk 时） |
| `payload` / `payloadSize` | 负载（**借用**输入缓冲内部指针，仅本次分发有效） |

## 3. 处理器（MessageHandler）

```cpp
using MessageHandler = std::function<void(ConnectionId id, int type,
                                          const char* payload, size_t payloadSize)>;
```

## 4. 使用流程

```cpp
bool CMyService::Initialize(const sc::CResolveContext& ctx)
{
    m_pNetwork.Reset(ctx.Resolve<sc::INetwork>());
    m_pRouter.Reset(ctx.Resolve<sc::IMessageRouter>());
    if (m_pRouter == nullptr) return false;

    m_pRouter->SetExtractor(MyProtocol::MakeMessageExtractor());  // ① 协议提取器
    m_pRouter->RegisterHandler(kCmdPing,
        [this](sc::ConnectionId id, int, const char*, size_t) { HandlePing(id); });
    m_pRouter->RegisterHandler(kCmdSubmit,
        [this](sc::ConnectionId id, int, const char* p, size_t n) { HandleSubmit(id, p, n); });
    return true;
}

void CMyService::OnData(sc::ConnectionId id, const char* data, size_t len)  // INetworkHandler 转发
{ if (m_pRouter != nullptr) m_pRouter->OnData(id, data, len); }
void CMyService::OnClose(sc::ConnectionId id)
{ if (m_pRouter != nullptr) m_pRouter->OnClose(id); }
```

业务不再维护 `std::map<ConnectionId, std::string>` 缓冲，也不再手写 `while(ParsePacket)` 循环。

## 5. 实现一个提取器（零拷贝）

提取器直接解析输入缓冲，负载借用缓冲内部指针：

```cpp
sc::MessageExtractor MyProtocol::MakeMessageExtractor()
{
    return [](const char* pData, size_t nLen) -> sc::ExtractedMessage
    {
        sc::ExtractedMessage result;
        result.result = sc::MessageParseResult::kNeedMore;
        if (pData == nullptr || nLen < kHeaderSize) return result;

        std::uint32_t len = /* 读网络序长度 */;
        if (len < 1 || len > kMaxPacketSize) { result.result = kInvalid; return result; }
        size_t total = kHeaderSize + len;
        if (nLen < total) return result;          // 半包

        result.result = sc::MessageParseResult::kOk;
        result.step = total;
        result.type = static_cast<unsigned char>(pData[kHeaderSize]);
        result.payload = pData + kHeaderSize + 1; // 借用输入缓冲
        result.payloadSize = len - 1;
        return result;
    };
}
```

参考实现：`ServerExample/Protocol/ExampleProtocol.cpp`、`LogServer/Protocol/LogProtocol.cpp`。

## 6. 线程安全

`OnData / OnClose` 与 `SetExtractor / RegisterHandler` 可跨线程安全调用（互斥锁保护）；
处理器在**锁外**调用，避免处理器内再次进入本模块造成死锁。同一连接的数据应串行进入（网络层保证）。
