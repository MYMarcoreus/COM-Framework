#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "Module/ScopedInterfacePtr.h"
#include "Infra/IAsyncExecutor.h"
#include "Module/Module.h"
#include "Module/InterfaceMap.h"
#include "Event/IEventDispatcher.h"

namespace sc {

/// @brief 事件分发器模块。
///
/// 线程安全：订阅 / 取消订阅 / 发布均可跨线程调用。
/// 发布时在锁外调用处理器，避免处理器内再次发布导致死锁。
/// 异步发布：Initialize 中从初始化上下文解析可选的 IAsyncExecutor
/// （需先注册异步执行器模块），PublishAsync 将事件投递到执行器线程处理，
/// 不阻塞发布线程；未配置执行器时退化为同步发布。
/// 可作为模块注册到 CModuleManager（IID_EventDispatcher）。
class CEventDispatcher : public CModule, public IEventDispatcher
{
public:
    CEventDispatcher();

    virtual ~CEventDispatcher();

    // 从初始化上下文解析可选的 IAsyncExecutor（缺失时 PublishAsync 退化为同步）。
    bool Initialize(const CResolveContext& ctx) override;

    // 生命周期：事件分发器无额外资源，直接成功。
    bool Start() override;
    void Stop() override;
    void Shutdown() override;

    // 订阅事件，返回订阅标识。
    SubscriptionId Subscribe(const EventType& strType, const EventHandler& fnHandler) override;

    // 根据订阅标识取消订阅。
    bool Unsubscribe(SubscriptionId nId) override;

    // 发布事件，同步调用所有订阅者。
    size_t Publish(const EventType& strType, const void* pData, size_t nSize) override;

    // 异步发布事件：投递到异步执行器处理；未配置执行器时退化为同步。
    size_t PublishAsync(const EventType& strType, const void* pData, size_t nSize) override;

    // 指定事件的订阅者数量。
    size_t SubscriberCount(const EventType& strType) const override;

    SC_DECLARE_INTERFACE_MAP();

private:
    struct Subscription
    {
        EventType type;
        EventHandler handler;

        Subscription() : type() {}
    };

    std::map<SubscriptionId, Subscription> m_mapSubscriptions;
    std::map<EventType, std::vector<SubscriptionId> > m_mapByType;
    ScopedInterfacePtr<IAsyncExecutor> m_pExecutor;
    mutable std::mutex m_mutex;
    SubscriptionId m_nNextId;
};

} // namespace sc
