#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "Component/Component.h"
#include "Event/IEventDispatcher.h"

namespace sc {

/// @brief 事件分发器组件。
///
/// 线程安全：订阅 / 取消订阅 / 发布均可跨线程调用。
/// 发布时在锁外调用处理器，避免处理器内再次发布导致死锁。
/// 可作为组件注册到 CComponentManager（IID_EventDispatcher）。
class CEventDispatcher : public CComponent, public IEventDispatcher
{
public:
    CEventDispatcher();

    virtual ~CEventDispatcher();

    // 订阅事件，返回订阅标识。
    SubscriptionId Subscribe(const EventType& type, const EventHandler& handler) override;

    // 根据订阅标识取消订阅。
    bool Unsubscribe(SubscriptionId id) override;

    // 发布事件，同步调用所有订阅者。
    size_t Publish(const EventType& type, const void* data, size_t size) override;

    // 指定事件的订阅者数量。
    size_t SubscriberCount(const EventType& type) const override;

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
