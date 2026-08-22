#include "Event/EventDispatcher.h"

#include "Module/ResolveContext.h"
#include "Module/InterfaceMap.h"

namespace sc {

// 接口映射表：暴露本类实现的接口（查表驱动 QueryInterface）。
SC_DEFINE_INTERFACE_MAP(CEventDispatcher, CModule, IEventDispatcher)

/// @brief 创建事件分发器。
CEventDispatcher::CEventDispatcher() : CModule("event"), m_nNextId(1)
{
}

/// @brief 销毁事件分发器。
CEventDispatcher::~CEventDispatcher()
{
}

/// @brief 从初始化上下文解析可选的 IAsyncExecutor。
///
/// @param ctx 初始化上下文（依赖注入）。
///
/// @return true（异步执行器缺失不视为失败，PublishAsync 退化为同步）。
bool CEventDispatcher::Initialize(const CResolveContext& ctx)
{
    m_pExecutor.Reset(ctx.Resolve<IAsyncExecutor>());
    return true;
}

/// @brief 模块启动（无额外资源，直接成功）。
bool CEventDispatcher::Start()
{
    return true;
}

/// @brief 模块停止（事件分发器无生命周期资源，无需处理）。
void CEventDispatcher::Stop()
{
}

/// @brief 模块关闭（事件分发器无生命周期资源，无需处理）。
void CEventDispatcher::Shutdown()
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

/// @brief 异步发布事件。
///
/// 将负载拷贝后投递到异步执行器线程处理，不阻塞发布线程。
/// 未配置异步执行器时退化为同步 Publish。
/// 任务捕获模块自持引用，保证事件处理期间模块存活。
///
/// @param type 事件类型。
/// @param data 事件负载（借用指针，会被拷贝）。
/// @param size 负载字节数。
///
/// @return 投递成功时返回调用时快照的订阅者数量；否则返回 0。
size_t CEventDispatcher::PublishAsync(const EventType& strType, const void* pData, size_t nSize)
{
    // ① 快照订阅者数量
    size_t nCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::map<EventType, std::vector<SubscriptionId> >::const_iterator it =
            m_mapByType.find(strType);
        if (it != m_mapByType.end())
        {
            nCount = it->second.size();
        }
    }

    // ② 未配置异步执行器：退化为同步发布
    if (m_pExecutor == nullptr)
    {
        return Publish(strType, pData, nSize);
    }

    // ③ 拷贝负载（异步返回后借用指针失效）
    std::vector<char> vecPayload;
    if (pData != nullptr && nSize > 0)
    {
        const char* pBegin = static_cast<const char*>(pData);
        vecPayload.assign(pBegin, pBegin + nSize);
    }

    // ④ 投递到执行器；捕获模块自持引用保证回调期间模块存活
    auto spSelf = Self<CEventDispatcher>();
    bool bPosted = m_pExecutor->Post(
        [spSelf, strType, vecPayload]()
        {
            if (spSelf)
            {
                spSelf->Publish(strType, vecPayload.empty() ? nullptr : vecPayload.data(),
                                vecPayload.size());
            }
        });
    return bPosted ? nCount : 0;
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

} // namespace sc
