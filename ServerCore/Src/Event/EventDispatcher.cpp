#include "Event/EventDispatcher.h"

#include <string>

namespace sc {

/// @brief 创建事件分发器。
CEventDispatcher::CEventDispatcher() : CModule("event"), m_nNextId(1)
{
}

/// @brief 销毁事件分发器。
CEventDispatcher::~CEventDispatcher()
{
}

/// @brief 订阅事件。
///
/// 返回递增的订阅标识，用于 Unsubscribe；处理器为空或达到上限时返回 0。
///
/// @param type    事件类型。
/// @param handler 事件处理器。
///
/// @return 订阅标识；失败返回 kInvalidSubscriptionId。
SubscriptionId CEventDispatcher::Subscribe(const EventType& strType, const EventHandler& fnHandler)
{
    if (!fnHandler)
    {
        return kInvalidSubscriptionId;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    SubscriptionId nId = m_nNextId++;
    Subscription sub;
    sub.type = strType;
    sub.handler = fnHandler;
    m_mapSubscriptions[nId] = sub;
    m_mapByType[strType].push_back(nId);
    return nId;
}

/// @brief 根据订阅标识取消订阅。
///
/// @param nId 订阅标识。
///
/// @return true 取消成功；false 标识无效。
bool CEventDispatcher::Unsubscribe(SubscriptionId nId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<SubscriptionId, Subscription>::iterator it = m_mapSubscriptions.find(nId);
    if (it == m_mapSubscriptions.end())
    {
        return false;
    }
    const EventType& type = it->second.type;
    std::vector<SubscriptionId>& ids = m_mapByType[type];
    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (ids[i] == nId)
        {
            ids.erase(ids.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    if (ids.empty())
    {
        m_mapByType.erase(type);
    }
    m_mapSubscriptions.erase(it);
    return true;
}

/// @brief 发布事件。
///
/// 在锁外同步调用所有订阅者，处理器数量即为返回值。
///
/// @param type 事件类型。
/// @param data 事件负载（借用指针，可为空）。
/// @param size 负载字节数。
///
/// @return 实际接收事件的处理器数量。
size_t CEventDispatcher::Publish(const EventType& strType, const void* pData, size_t nSize)
{
    std::vector<EventHandler> vecTargets;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::map<EventType, std::vector<SubscriptionId> >::const_iterator it = m_mapByType.find(strType);
        if (it == m_mapByType.end())
        {
            return 0;
        }
        const std::vector<SubscriptionId>& ids = it->second;
        vecTargets.reserve(ids.size());
        for (size_t i = 0; i < ids.size(); ++i)
        {
            std::map<SubscriptionId, Subscription>::const_iterator sit = m_mapSubscriptions.find(ids[i]);
            if (sit != m_mapSubscriptions.end())
            {
                vecTargets.push_back(sit->second.handler);
            }
        }
    }
    Event event(strType, pData, nSize);
    for (size_t i = 0; i < vecTargets.size(); ++i)
    {
        if (vecTargets[i])
        {
            vecTargets[i](event);
        }
    }
    return vecTargets.size();
}

/// @brief 指定事件的订阅者数量。
///
/// @param strType 事件类型。
///
/// @return 订阅者数量；无订阅返回 0。
size_t CEventDispatcher::SubscriberCount(const EventType& strType) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<EventType, std::vector<SubscriptionId> >::const_iterator it = m_mapByType.find(strType);
    if (it == m_mapByType.end())
    {
        return 0;
    }
    return it->second.size();
}

/// @brief 接口查询实现。
bool CEventDispatcher::QueryInterfaceImpl(const InterfaceId& iid, void** ppv)
{
    if (iid == IID_EventDispatcher())
    {
        *ppv = static_cast<IEventDispatcher*>(this);
        return true;
    }
    return CModule::QueryInterfaceImpl(iid, ppv);
}

} // namespace sc
