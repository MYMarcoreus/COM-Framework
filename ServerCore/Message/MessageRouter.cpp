#include "Message/MessageRouter.h"

#include "Module/InterfaceMap.h"
#include <string>

namespace sc {

// 接口映射表：暴露本类实现的接口（查表驱动 QueryInterface）。
SC_DEFINE_INTERFACE_MAP(CMessageRouter, CModule, IMessageRouter)

/// @brief 创建消息路由器。
CMessageRouter::CMessageRouter() : CModule("message-router"), m_nNextId(1)
{
}

/// @brief 销毁消息路由器。
CMessageRouter::~CMessageRouter()
{
}

/// @brief 初始化模块（无配置依赖，直接成功）。
bool CMessageRouter::Initialize(const CResolveContext& /*ctx*/)
{
    return true;
}

/// @brief 模块启动（无独立启动资源）。
bool CMessageRouter::Start()
{
    return true;
}

/// @brief 模块停止（消息缓冲由各连接 OnClose 清理）。
void CMessageRouter::Stop()
{
}

/// @brief 模块关闭（消息缓冲由析构释放）。
void CMessageRouter::Shutdown()
{
}

/// @brief 设置消息提取器。
///
/// @param extractor 业务提供的提取器（协议相关）。
void CMessageRouter::SetExtractor(const MessageExtractor& fnExtractor)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_fnExtractor = fnExtractor;
}

/// @brief 注册消息处理器。
///
/// @param nType    消息类型。
/// @param fnHandler 处理器。
///
/// @return 订阅标识；失败返回 kInvalidSubscriptionId。
SubscriptionId CMessageRouter::RegisterHandler(int nType, const MessageHandler& fnHandler)
{
    if (!fnHandler)
    {
        return kInvalidSubscriptionId;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    SubscriptionId nId = m_nNextId++;
    HandlerEntry entry;
    entry.type = nType;
    entry.handler = fnHandler;
    m_mapHandlers[nId] = entry;
    m_mapByType[nType].push_back(nId);
    return nId;
}

/// @brief 反注册消息处理器。
///
/// @param nId 订阅标识。
///
/// @return true 反注册成功；false 标识无效。
bool CMessageRouter::UnregisterHandler(SubscriptionId nId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<SubscriptionId, HandlerEntry>::iterator it = m_mapHandlers.find(nId);
    if (it == m_mapHandlers.end())
    {
        return false;
    }
    int nType = it->second.type;
    std::vector<SubscriptionId>& ids = m_mapByType[nType];
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
        m_mapByType.erase(nType);
    }
    m_mapHandlers.erase(it);
    return true;
}

/// @brief 数据入口。
///
/// 追加到连接缓冲，循环提取完整消息并分发。
///
/// @note 同一连接的数据应串行进入（网络层保证）。
void CMessageRouter::OnData(ConnectionId nId, const char* pData, size_t nLen)
{
    if (pData == nullptr || nLen == 0)
    {
        return;
    }
    // ① 取当前缓冲并追加（移除后再处理，避免锁外共享）
    std::string strPending;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::map<ConnectionId, std::string>::iterator it = m_mapBuffers.find(nId);
        if (it != m_mapBuffers.end())
        {
            strPending = it->second;
            m_mapBuffers.erase(it);
        }
        strPending.append(pData, nLen);
    }

    // ② 锁外提取并分发
    size_t nConsumed = 0;
    while (nConsumed < strPending.size())
    {
        if (!m_fnExtractor)
        {
            break;
        }
        ExtractedMessage extracted = m_fnExtractor(strPending.data() + nConsumed, strPending.size() - nConsumed);
        if (extracted.result == MessageParseResult::kNeedMore)
        {
            break;
        }
        if (extracted.result == MessageParseResult::kInvalid || extracted.step == 0)
        {
            // 协议非法或提取器异常：丢弃剩余数据
            nConsumed = strPending.size();
            break;
        }
        nConsumed += extracted.step;
        Dispatch(nId, extracted.type, extracted.payload, extracted.payloadSize);
    }

    // ③ 剩余数据放回缓冲
    std::string strRemain = strPending.substr(nConsumed);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (strRemain.empty())
        {
            m_mapBuffers.erase(nId);
        }
        else
        {
            m_mapBuffers[nId] = strRemain;
        }
    }
}

/// @brief 连接关闭时清理缓冲。
void CMessageRouter::OnClose(ConnectionId nId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_mapBuffers.erase(nId);
}

/// @brief 按类型分发一条消息。
///
/// 在锁外调用处理器，避免处理器内再次调用本模块时死锁。
void CMessageRouter::Dispatch(ConnectionId nId, int nType,
                             const char* pPayload, size_t nPayloadSize)
{
    std::vector<MessageHandler> vecTargets;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::map<int, std::vector<SubscriptionId> >::const_iterator it = m_mapByType.find(nType);
        if (it == m_mapByType.end())
        {
            return;
        }
        const std::vector<SubscriptionId>& ids = it->second;
        vecTargets.reserve(ids.size());
        for (size_t i = 0; i < ids.size(); ++i)
        {
            std::map<SubscriptionId, HandlerEntry>::const_iterator sit = m_mapHandlers.find(ids[i]);
            if (sit != m_mapHandlers.end())
            {
                vecTargets.push_back(sit->second.handler);
            }
        }
    }
    for (size_t i = 0; i < vecTargets.size(); ++i)
    {
        if (vecTargets[i])
        {
            vecTargets[i](nId, nType, pPayload, nPayloadSize);
        }
    }
}

} // namespace sc
