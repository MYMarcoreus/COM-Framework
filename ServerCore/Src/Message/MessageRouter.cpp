#include "Message/MessageRouter.h"

#include <string>

namespace sc {

/// @brief 创建消息路由器。
CMessageRouter::CMessageRouter() : m_nNextId(1)
{
}

/// @brief 销毁消息路由器。
CMessageRouter::~CMessageRouter()
{
}

/// @brief 设置消息提取器。
///
/// @param extractor 业务提供的提取器（协议相关）。
void CMessageRouter::SetExtractor(const MessageExtractor& extractor)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_fnExtractor = extractor;
}

/// @brief 注册消息处理器。
///
/// @param type    消息类型。
/// @param handler 处理器。
///
/// @return 订阅标识；失败返回 kInvalidSubscriptionId。
SubscriptionId CMessageRouter::RegisterHandler(int type, const MessageHandler& handler)
{
    if (!handler)
    {
        return kInvalidSubscriptionId;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    SubscriptionId id = m_nNextId++;
    HandlerEntry entry;
    entry.type = type;
    entry.handler = handler;
    m_mapHandlers[id] = entry;
    m_mapByType[type].push_back(id);
    return id;
}

/// @brief 反注册消息处理器。
///
/// @param id 订阅标识。
///
/// @return true 反注册成功；false 标识无效。
bool CMessageRouter::UnregisterHandler(SubscriptionId id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<SubscriptionId, HandlerEntry>::iterator it = m_mapHandlers.find(id);
    if (it == m_mapHandlers.end())
    {
        return false;
    }
    int type = it->second.type;
    std::vector<SubscriptionId>& ids = m_mapByType[type];
    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (ids[i] == id)
        {
            ids.erase(ids.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    if (ids.empty())
    {
        m_mapByType.erase(type);
    }
    m_mapHandlers.erase(it);
    return true;
}

/// @brief 数据入口。
///
/// 追加到连接缓冲，循环提取完整消息并分发。
///
/// @note 同一连接的数据应串行进入（网络层保证）。
void CMessageRouter::OnData(ConnectionId id, const char* data, size_t len)
{
    if (data == nullptr || len == 0)
    {
        return;
    }
    // ① 取当前缓冲并追加（移除后再处理，避免锁外共享）
    std::string pending;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::map<ConnectionId, std::string>::iterator it = m_mapBuffers.find(id);
        if (it != m_mapBuffers.end())
        {
            pending = it->second;
            m_mapBuffers.erase(it);
        }
        pending.append(data, len);
    }

    // ② 锁外提取并分发
    size_t consumed = 0;
    while (consumed < pending.size())
    {
        if (!m_fnExtractor)
        {
            break;
        }
        int type = 0;
        const char* payload = nullptr;
        size_t payloadSize = 0;
        size_t step = 0;
        MessageParseResult result = m_fnExtractor(
            pending.data() + consumed, pending.size() - consumed,
            &step, &type, &payload, &payloadSize);
        if (result == MessageParseResult::kNeedMore)
        {
            break;
        }
        if (result == MessageParseResult::kInvalid || step == 0)
        {
            // 协议非法或提取器异常：丢弃剩余数据
            consumed = pending.size();
            break;
        }
        consumed += step;
        Dispatch(id, type, payload, payloadSize);
    }

    // ③ 剩余数据放回缓冲
    std::string remain = pending.substr(consumed);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (remain.empty())
        {
            m_mapBuffers.erase(id);
        }
        else
        {
            m_mapBuffers[id] = remain;
        }
    }
}

/// @brief 连接关闭时清理缓冲。
void CMessageRouter::OnClose(ConnectionId id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_mapBuffers.erase(id);
}

/// @brief 按类型分发一条消息。
///
/// 在锁外调用处理器，避免处理器内再次调用本组件时死锁。
void CMessageRouter::Dispatch(ConnectionId id, int type,
                             const char* payload, size_t payloadSize)
{
    std::vector<MessageHandler> targets;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::map<int, std::vector<SubscriptionId> >::const_iterator it = m_mapByType.find(type);
        if (it == m_mapByType.end())
        {
            return;
        }
        const std::vector<SubscriptionId>& ids = it->second;
        targets.reserve(ids.size());
        for (size_t i = 0; i < ids.size(); ++i)
        {
            std::map<SubscriptionId, HandlerEntry>::const_iterator sit = m_mapHandlers.find(ids[i]);
            if (sit != m_mapHandlers.end())
            {
                targets.push_back(sit->second.handler);
            }
        }
    }
    for (size_t i = 0; i < targets.size(); ++i)
    {
        if (targets[i])
        {
            targets[i](id, type, payload, payloadSize);
        }
    }
}

/// @brief 接口查询实现。
bool CMessageRouter::QueryInterfaceImpl(const InterfaceId& iid, void** ppv)
{
    if (std::string(iid) == std::string(IID_IMessageRouter()))
    {
        *ppv = static_cast<IMessageRouter*>(this);
        return true;
    }
    return CComponent::QueryInterfaceImpl(iid, ppv);
}

} // namespace sc
