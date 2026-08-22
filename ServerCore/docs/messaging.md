# 消息流水线（CMessageRouter）

ServerCore 提供协议无关的消息流水线：**缓冲 / 半包 / 粘包切分 / 按类型分发**全部
下沉到框架，业务只提供"提取器"与"命令处理器"。

## 接口（IMessageRouter）

```cpp
class IMessageRouter : public virtual IUnknown
{
    // 设置消息提取器（协议相关，业务提供）
    virtual void SetExtractor(const MessageExtractor& fnExtractor) = 0;

    // 注册按类型的处理器，返回订阅标识
    virtual SubscriptionId RegisterHandler(int nType, const MessageHandler& fnHandler) = 0;

    // 反注册处理器
    virtual bool UnregisterHandler(SubscriptionId nId) = 0;

    // 数据入口（网络层 OnData 调用）
    virtual void OnData(ConnectionId nId, const char* pData, size_t nLen) = 0;

    // 连接关闭清理（网络层 OnClose 调用）
    virtual void OnClose(ConnectionId nId) = 0;
};
```

## 提取器（MessageExtractor）

```cpp
using MessageExtractor =
    std::function<ExtractedMessage(const char* data, size_t len)>;
```

业务根据协议从流起始处提取一条完整消息，返回结构体：

| 字段 | 含义 |
|---|---|
| `result` | `kNeedMore`（半包）/ `kOk`（完整）/ `kInvalid`（非法） |
| `step` | 本条消息消耗的字节数（kOk 时） |
| `type` | 消息类型（kOk 时） |
| `payload` / `payloadSize` | 负载（借用输入缓冲内部指针，仅本次分发有效） |

## 处理器（MessageHandler）

```cpp
using MessageHandler = std::function<void(ConnectionId id, int type,
                                          const char* payload, size_t payloadSize)>;
```

## 使用流程（以业务服务为例）

```cpp
bool CMyService::Initialize(const sc::CResolveContext& ctx)
{
    m_pNetwork.Reset(ctx.Resolve<sc::INetwork>());
    m_pRouter.Reset(ctx.Resolve<sc::IMessageRouter>());
    if (m_pRouter == nullptr) return false;

    // ① 设置协议提取器（业务协议）
    m_pRouter->SetExtractor(MyProtocol::MakeMessageExtractor());

    // ② 注册命令处理器
    m_pRouter->RegisterHandler(kCmdPing,
        [this](sc::ConnectionId id, int, const char*, size_t) { HandlePing(id); });
    m_pRouter->RegisterHandler(kCmdSubmit,
        [this](sc::ConnectionId id, int, const char* p, size_t n)
        { HandleSubmit(id, p, n); });
    return true;
}

// INetworkHandler 只需转发
void CMyService::OnData(sc::ConnectionId id, const char* data, size_t len)
{
    if (m_pRouter != nullptr) m_pRouter->OnData(id, data, len);
}
void CMyService::OnClose(sc::ConnectionId id)
{
    if (m_pRouter != nullptr) m_pRouter->OnClose(id);
}
```

业务不再维护 `std::map<ConnectionId, std::string>` 缓冲，也不再手写
`while(ParsePacket)` 循环。

## 实现一个提取器（零拷贝）

提取器应直接解析输入缓冲，负载借用缓冲内部指针：

```cpp
sc::MessageExtractor MyProtocol::MakeMessageExtractor()
{
    return [](const char* pData, size_t nLen) -> sc::ExtractedMessage
    {
        sc::ExtractedMessage result;
        result.result = sc::MessageParseResult::kNeedMore;
        if (pData == nullptr || nLen < kHeaderSize) return result;

        // 读长度字段 → 校验 → 检查完整 → 返回借用负载
        std::uint32_t len = /* 读网络序长度 */;
        if (len < 1 || len > kMaxPacketSize)
        {
            result.result = sc::MessageParseResult::kInvalid;
            return result;
        }
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

参考实现：`Demo/Protocol/DemoProtocol.cpp`、`LogServer/Protocol/LogProtocol.cpp`
的 `MakeMessageExtractor()`。

## 线程安全

`CMessageRouter` 内部按连接维护缓冲，`OnData` / `OnClose` 与
`SetExtractor` / `RegisterHandler` 可跨线程安全调用（互斥锁保护）；
处理器在锁外调用，避免处理器内再次进入本模块造成死锁。
同一连接的数据应串行进入（网络层保证）。
