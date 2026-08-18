#pragma once

#include <cstddef>

#include "Component/IUnknown.h"
#include "Event/EventTypes.h"

namespace sc {

/// @brief 获取 IEventDispatcher 接口标识。
inline const InterfaceId& IID_EventDispatcher()
{
    static const InterfaceId iid = "sc::IEventDispatcher";
    return iid;
}

/// @brief 事件分发器接口（COM 风格：继承 IUnknown）。
///
/// 支持事件订阅、取消订阅与发布，用于模块之间的解耦通信：
/// 发布者与订阅者互不依赖，仅通过事件类型耦合。
class IEventDispatcher : public virtual IUnknown
{
public:
    virtual ~IEventDispatcher() {}

    // 订阅事件，返回订阅标识（用于取消订阅）；失败返回 kInvalidSubscriptionId。
    virtual SubscriptionId Subscribe(const EventType& strType, const EventHandler& fnHandler) = 0;

    // 根据订阅标识取消订阅。
    virtual bool Unsubscribe(SubscriptionId nId) = 0;

    // 发布事件，同步调用所有订阅者；返回实际接收的处理器数量。
    virtual size_t Publish(const EventType& strType, const void* pData, size_t nSize) = 0;

    // 指定事件的订阅者数量。
    virtual size_t SubscriberCount(const EventType& strType) const = 0;
};

} // namespace sc
