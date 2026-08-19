#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "Module/Module.h"
#include "Event/IEventDispatcher.h"

namespace sc {

/// @brief 事件分发器模块。
///
/// 线程安全：订阅 / 取消订阅 / 发布均可跨线程调用。
/// 发布时在锁外调用处理器，避免处理器内再次发布导致死锁。
/// 可作为模块注册到 CModuleManager（IID_EventDispatcher）。
class CEventDispatcher : public CModule, public IEventDispatcher
{
public:
    CEventDispatcher();

    virtual ~CEventDispatcher();

    // 订阅事件，返回订阅标识。
    SubscriptionId Subscribe(const EventType& strType, const EventHandler& fnHandler) override;

    // 根据订阅标识取消订阅。
    bool Unsubscribe(SubscriptionId nId) override;

    // 发布事件，同步调用所有订阅者。
    size_t Publish(const EventType& strType, const void* pData, size_t nSize) override;

    // 指定事件的订阅者数量。
    size_t SubscriberCount(const EventType& strType) const override;

protected:
    // 接口查询实现。
    bool QueryInterfaceImpl(const InterfaceId& iid, void** ppv) override;

private:
    struct Subscription
    {
        EventType type;
        EventHandler handler;

        Subscription() : type() {}
    };

    std::map<SubscriptionId, Subscription> m_mapSubscriptions;
    std::map<EventType, std::vector<SubscriptionId> > m_mapByType;
    mutable std::mutex m_mutex;
    SubscriptionId m_nNextId;
};

} // namespace sc
