#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "Module/Module.h"
#include "Message/IMessageRouter.h"

namespace sc {

/// @brief 消息路由器模块。
///
/// 按连接维护接收缓冲，使用业务提供的提取器切分完整消息，并按类型分发。
///
/// @note 同一连接的数据应串行进入（网络层保证），OnData/OnClose 与
///       SetExtractor/RegisterHandler 可跨线程安全调用。
class CMessageRouter : public CModule, public IMessageRouter
{
public:
    CMessageRouter();

    virtual ~CMessageRouter();

    // 设置消息提取器。
    void SetExtractor(const MessageExtractor& fnExtractor) override;

    // 注册消息处理器。
    SubscriptionId RegisterHandler(int nType, const MessageHandler& fnHandler) override;

    // 反注册消息处理器。
    bool UnregisterHandler(SubscriptionId nId) override;

    // 数据入口。
    void OnData(ConnectionId nId, const char* pData, size_t nLen) override;

    // 连接关闭清理。
    void OnClose(ConnectionId nId) override;

protected:
    // 接口查询实现。
    bool QueryInterfaceImpl(const InterfaceId& iid, void** ppv) override;

private:
    struct HandlerEntry
    {
        int type;
        MessageHandler handler;

        HandlerEntry() : type(0) {}
    };

    // 按类型分发一条消息。
    void Dispatch(ConnectionId nId, int nType, const char* pPayload, size_t nPayloadSize);

    MessageExtractor m_fnExtractor;
    std::map<ConnectionId, std::string> m_mapBuffers;
    std::map<SubscriptionId, HandlerEntry> m_mapHandlers;
    std::map<int, std::vector<SubscriptionId> > m_mapByType;
    mutable std::mutex m_mutex;
    SubscriptionId m_nNextId;
};

} // namespace sc
