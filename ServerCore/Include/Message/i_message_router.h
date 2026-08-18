#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "Common/types.h"
#include "Component/i_unknown.h"
#include "Event/event_types.h"

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

/// @brief 消息提取器（协议相关，由业务提供）。
///
/// 从数据流起始处提取一条完整消息：
/// - 成功返回 kOk，通过 step 给出消耗字节数，outType/outPayload/outPayloadSize
///   给出消息类型与负载（负载不含消息头）；
/// - 数据不足返回 kNeedMore；
/// - 协议非法返回 kInvalid。
using MessageExtractor = std::function<MessageParseResult(
    const char* data, size_t len, size_t* step,
    int* outType, const char** outPayload, size_t* outPayloadSize)>;

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
    virtual void SetExtractor(const MessageExtractor& extractor) = 0;

    // 注册消息处理器（按类型），返回订阅标识。
    virtual SubscriptionId RegisterHandler(int type, const MessageHandler& handler) = 0;

    // 反注册消息处理器。
    virtual bool UnregisterHandler(SubscriptionId id) = 0;

    // 数据入口（供 INetworkHandler::OnData 调用）。
    virtual void OnData(ConnectionId id, const char* data, size_t len) = 0;

    // 连接关闭时清理缓冲（供 INetworkHandler::OnClose 调用）。
    virtual void OnClose(ConnectionId id) = 0;
};

} // namespace sc
