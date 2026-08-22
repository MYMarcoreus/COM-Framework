#pragma once

#include <cstddef>

#include "Module/IUnknown.h"
#include "Module/InterfaceDecl.h"
#include "Event/EventTypes.h"

namespace sc {

/// @brief 事件分发器接口（COM 风格：继承 IUnknown）。
///
/// 支持事件订阅、取消订阅与发布，用于模块之间的解耦通信：
/// 发布者与订阅者互不依赖，仅通过事件类型耦合。
SC_INTERFACE(IEventDispatcher, "sc::IEventDispatcher", "3da6cfa3-000a-49cd-9510-0df2984983e0")
{
public:
    virtual ~IEventDispatcher() {}

    // 订阅事件，返回订阅标识（用于取消订阅）；失败返回 kInvalidSubscriptionId。
    virtual SubscriptionId Subscribe(const EventType& strType, const EventHandler& fnHandler) = 0;

    // 根据订阅标识取消订阅。
    virtual bool Unsubscribe(SubscriptionId nId) = 0;

    // 发布事件，同步调用所有订阅者；返回实际接收的处理器数量。
    virtual size_t Publish(const EventType& strType, const void* pData, size_t nSize) = 0;

    // 异步发布事件：将负载拷贝后投递到异步执行器处理（不阻塞当前线程）。
    // 返回值为调用时的订阅者数量（参考值）；未配置异步执行器时退化为同步 Publish。
    virtual size_t PublishAsync(const EventType& strType, const void* pData, size_t nSize) = 0;

    // 指定事件的订阅者数量。
    virtual size_t SubscriberCount(const EventType& strType) const = 0;
};

} // namespace sc
