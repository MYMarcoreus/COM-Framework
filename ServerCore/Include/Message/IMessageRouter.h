#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "Common/Types.h"
#include "Component/IUnknown.h"
#include "Event/EventTypes.h"

namespace sc {

/// @brief 获取 IMessageRouter 接口标识。
inline const InterfaceId& IID_IMessageRouter()
{
    static const InterfaceId iid = "sc::IMessageRouter";
    return iid;
}

/// @brief 消息解析结果。
enum class MessageParseResult
{
    kNeedMore, // 数据不足，等待更多
    kOk,       // 提取出一条完整消息
    kInvalid   // 数据非法
};

/// @brief 单条消息提取结果。
///
/// 通过结构体一次返回全部输出，避免 C 风格 out 指针 / 二重指针参数。
/// 仅在 result == kOk 时，type / payload / payloadSize / step 有效；
/// payload 为借用指针，指向输入缓冲内部，本次提取后即失效，不得长期保存。
struct ExtractedMessage
{
    MessageParseResult result; // 解析结果
    size_t step;               // 消耗字节数（kOk 时有效）
    int type;                  // 消息类型（kOk 时有效）
    const char* payload;       // 消息负载（借用指针，不含消息头，kOk 时有效）
    size_t payloadSize;        // 负载字节数（kOk 时有效）
};

/// @brief 消息提取器（协议相关，由业务提供）。
///
/// 从数据流起始处提取一条完整消息，返回提取结果结构体：
/// - 成功返回 kOk，step 给出消耗字节数，type / payload / payloadSize
///   给出消息类型与负载（负载不含消息头）；
/// - 数据不足返回 kNeedMore；
/// - 协议非法返回 kInvalid。
using MessageExtractor = std::function<ExtractedMessage(const char* data, size_t len)>;

/// @brief 消息处理器。
///
/// @param id         连接标识。
/// @param type       消息类型。
/// @param payload    消息负载（借用指针，回调期间有效）。
/// @param payloadSize 负载字节数。
using MessageHandler = std::function<void(ConnectionId id, int type,
                                          const char* payload, size_t payloadSize)>;

/// @brief 基础消息分发接口（COM 风格：继承 IUnknown）。
///
/// 提供协议无关的消息分发基础设施：按连接维护接收缓冲，通过业务提供的
/// 提取器切分出完整消息，并按消息类型路由到对应处理器。
/// 与具体协议解耦（提取器属于业务层）。
class IMessageRouter : public virtual IUnknown
{
public:
    virtual ~IMessageRouter() {}

    // 设置消息提取器（协议相关，业务提供）。
    virtual void SetExtractor(const MessageExtractor& fnExtractor) = 0;

    // 注册消息处理器（按类型），返回订阅标识。
    virtual SubscriptionId RegisterHandler(int nType, const MessageHandler& fnHandler) = 0;

    // 反注册消息处理器。
    virtual bool UnregisterHandler(SubscriptionId nId) = 0;

    // 数据入口（供 INetworkHandler::OnData 调用）。
    virtual void OnData(ConnectionId nId, const char* pData, size_t nLen) = 0;

    // 连接关闭时清理缓冲（供 INetworkHandler::OnClose 调用）。
    virtual void OnClose(ConnectionId nId) = 0;
};

} // namespace sc
