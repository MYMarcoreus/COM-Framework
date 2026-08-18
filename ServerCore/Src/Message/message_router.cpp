#include "Message/message_router.h"

#include <string>

namespace sc {

/// @brief 创建消息路由器。
CMessageRouter::CMessageRouter() : nextId_(1)
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
    std::lock_guard<std::mutex> lock(mutex_);
    extractor_ = extractor;
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
    std::lock_guard<std::mutex> lock(mutex_);
    SubscriptionId id = nextId_++;
    HandlerEntry entry;
    entry.type = type;
    entry.handler = handler;
    handlers_[id] = entry;
    byType_[type].push_back(id);
    return id;
}

/// @brief 反注册消息处理器。
///
/// @param id 订阅标识。
///
/// @return true 反注册成功；false 标识无效。
bool CMessageRouter::UnregisterHandler(SubscriptionId id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<SubscriptionId, HandlerEntry>::iterator it = handlers_.find(id);
    if (it == handlers_.end())
    {
        return false;
    }
    int type = it->second.type;
    std::vector<SubscriptionId>& ids = byType_[type];
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
        byType_.erase(type);
    }
    handlers_.erase(it);
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
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<ConnectionId, std::string>::iterator it = buffers_.find(id);
        if (it != buffers_.end())
        {
            pending = it->second;
            buffers_.erase(it);
        }
        pending.append(data, len);
    }

    // ② 锁外提取并分发
    size_t consumed = 0;
    while (consumed < pending.size())
    {
        if (!extractor_)
        {
            break;
        }
        int type = 0;
        const char* payload = nullptr;
        size_t payloadSize = 0;
        size_t step = 0;
        MessageParseResult result = extractor_(
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
        std::lock_guard<std::mutex> lock(mutex_);
        if (remain.empty())
        {
            buffers_.erase(id);
        }
        else
        {
            buffers_[id] = remain;
        }
    }
}

/// @brief 连接关闭时清理缓冲。
void CMessageRouter::OnClose(ConnectionId id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    buffers_.erase(id);
}

/// @brief 按类型分发一条消息。
///
/// 在锁外调用处理器，避免处理器内再次调用本组件时死锁。
void CMessageRouter::Dispatch(ConnectionId id, int type,
                             const char* payload, size_t payloadSize)
{
    std::vector<MessageHandler> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<int, std::vector<SubscriptionId> >::const_iterator it = byType_.find(type);
        if (it == byType_.end())
        {
            return;
        }
        const std::vector<SubscriptionId>& ids = it->second;
        targets.reserve(ids.size());
        for (size_t i = 0; i < ids.size(); ++i)
        {
            std::map<SubscriptionId, HandlerEntry>::const_iterator sit = handlers_.find(ids[i]);
            if (sit != handlers_.end())
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
