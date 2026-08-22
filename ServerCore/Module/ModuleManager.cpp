#include "Module/ModuleManager.h"

#include <chrono>
#include <deque>

#include "Module/ResolveContext.h"
#include "Module/Module.h"

namespace sc {

/// @brief 设置模块生命周期状态。
///
/// @note 状态由管理器驱动；通过 friend 访问 CModule::SetState。
/// 非 CModule 派生的模块实现无法设置状态（保持默认 kCreated）。
void CModuleManager::SetModuleState(IModule* pModule, ModuleState state)
{
    CModule* pImpl = dynamic_cast<CModule*>(pModule);
    if (pImpl != nullptr)
    {
        pImpl->SetState(state);
    }
}

/// @brief 创建模块管理器。
CModuleManager::CModuleManager()
{
}

/// @brief 销毁模块管理器，释放所有已注册模块。
CModuleManager::~CModuleManager()
{
    Clear();
}

/// @brief 注册模块（接管型，按名字）。
///
/// 直接接管调用方 new 出来的模块的创建者引用（不额外 AddRef）；
/// 成功时管理器持有该引用（Clear/Unregister 时 Release 归零析构），
/// 失败（名字为空/重复）时管理器负责 Release 并返回 false，调用方无需释放。
///
/// @param module 刚创建、尚未额外 AddRef 的模块接口指针。
///
/// @return true 注册成功并接管；false 注册失败（已释放）。
bool CModuleManager::RegisterModule(IModule* pModule)
{
    if (pModule == nullptr)
    {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const char* strName = pModule->GetName();
    if (strName == nullptr || strName[0] == '\0')
    {
        pModule->Release();
        return false;
    }
    if (m_mapIndexByName.find(strName) != m_mapIndexByName.end())
    {
        pModule->Release(); // 名称重复：释放
        return false;
    }
    m_mapIndexByName[strName] = m_vecModules.size();
    m_vecModules.push_back(Entry(pModule)); // 接管引用（不 AddRef）
    return true;
}

/// @brief 注册模块（接管型，按接口标识）。
///
/// 用于服务定位（如按 INetwork 获取网络模块）；同一接口标识支持多实例注册，
/// 服务定位时 GetModuleByIid 返回首个实例。
/// 语义与按名字版本一致：接管创建者引用，失败时自行 Release。
///
/// @param iid 接口标识。
/// @param module 刚创建、尚未额外 AddRef 的模块接口指针。
///
/// @return true 注册成功并接管；false 注册失败（已释放）。
bool CModuleManager::RegisterModule(const InterfaceId& iid, IModule* pModule)
{
    if (!iid.IsValid() || pModule == nullptr)
    {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // 同一接口支持多实例：直接插入（不覆盖已有实例）。
    m_mapIndexByIid.insert(std::make_pair(iid, m_vecModules.size()));
    m_vecModules.push_back(Entry(pModule, iid)); // 接管引用（不 AddRef）
    return true;
}

/// @brief 根据名称获取模块。
///
/// @param strName 模块名称。
///
/// @return 借用指针（不增加引用计数）；未找到返回 nullptr。
IModule* CModuleManager::GetModule(const char* strName) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::map<std::string, size_t>::const_iterator it = m_mapIndexByName.find(strName);
    if (it == m_mapIndexByName.end())
    {
        return nullptr;
    }
    return m_vecModules[it->second].module;
}

/// @brief 根据接口标识获取模块。
///
/// @param iid 接口标识。
///
/// @return 借用指针（不增加引用计数）；未找到返回 nullptr，
///         同一接口多实例时返回首个注册的模块。
IModule* CModuleManager::GetModuleByIid(const InterfaceId& iid) const
{
    if (!iid.IsValid())
    {
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::multimap<InterfaceId, size_t>::const_iterator it = m_mapIndexByIid.find(iid);
    if (it == m_mapIndexByIid.end())
    {
        return nullptr;
    }
    return m_vecModules[it->second].module;
}

/// @brief 根据接口标识获取全部模块（支持多实例）。
///
/// @param iid 接口标识。
///
/// @return 借用指针列表（不增加引用计数），按注册顺序。
std::vector<IModule*> CModuleManager::GetModulesByIid(const InterfaceId& iid) const
{
    std::vector<IModule*> vecResult;
    if (!iid.IsValid())
    {
        return vecResult;
    }
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::pair<std::multimap<InterfaceId, size_t>::const_iterator,
              std::multimap<InterfaceId, size_t>::const_iterator> range =
        m_mapIndexByIid.equal_range(iid);
    for (std::multimap<InterfaceId, size_t>::const_iterator it = range.first;
         it != range.second; ++it)
    {
        vecResult.push_back(m_vecModules[it->second].module);
    }
    return vecResult;
}

/// @brief 指定接口标识的模块数量。
///
/// @param iid 接口标识。
///
/// @return 模块数量（多实例注册后 > 1；未注册返回 0）。
size_t CModuleManager::ModuleCountByIid(const InterfaceId& iid) const
{
    return GetModulesByIid(iid).size();
}

/// @brief 查询模块当前状态。
///
/// @param strName 模块名称。
///
/// @return 模块状态；未找到返回 kCreated。
ModuleState CModuleManager::GetModuleState(const char* strName) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::map<std::string, size_t>::const_iterator it = m_mapIndexByName.find(strName);
    if (it == m_mapIndexByName.end())
    {
        return ModuleState::kCreated;
    }
    return m_vecModules[it->second].module->GetState();
}

/// @brief 反注册模块。
///
/// 移除模块并释放引用（锁外释放，避免模块析构时重入）。
///
/// @param strName 模块名称。
///
/// @return true 移除成功；false 未找到。
bool CModuleManager::UnregisterModule(const char* strName)
{
    IModule* pModule = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        std::map<std::string, size_t>::iterator it = m_mapIndexByName.find(strName);
        if (it == m_mapIndexByName.end())
        {
            return false;
        }
        size_t nIndex = it->second;
        pModule = m_vecModules[nIndex].module;
        m_vecModules.erase(m_vecModules.begin() + static_cast<std::ptrdiff_t>(nIndex));
        // 向量删除后重建名称与接口索引
        m_mapIndexByName.clear();
        m_mapIndexByIid.clear();
        for (size_t i = 0; i < m_vecModules.size(); ++i)
        {
            if (m_vecModules[i].iid.IsValid())
            {
                m_mapIndexByIid.insert(std::make_pair(m_vecModules[i].iid, i));
            }
            else
            {
                const char* strName = m_vecModules[i].module->GetName();
                if (strName != nullptr && strName[0] != '\0')
                {
                    m_mapIndexByName[strName] = i;
                }
            }
        }
    }
    pModule->Release();
    return true;
}

/// @brief 反注册接口标识对应的模块。
///
/// 移除模块并释放引用（锁外释放，避免模块析构时重入）。
///
/// @param iid 接口标识。
///
/// @return true 移除成功；false 未找到。
bool CModuleManager::UnregisterModuleByIid(const InterfaceId& iid)
{
    if (!iid.IsValid())
    {
        return false;
    }
    IModule* pModule = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        std::map<InterfaceId, size_t>::iterator it = m_mapIndexByIid.find(iid);
        if (it == m_mapIndexByIid.end())
        {
            return false;
        }
        size_t nIndex = it->second;
        pModule = m_vecModules[nIndex].module;
        m_vecModules.erase(m_vecModules.begin() + static_cast<std::ptrdiff_t>(nIndex));
        // 向量删除后重建名称与接口索引
        m_mapIndexByName.clear();
        m_mapIndexByIid.clear();
        for (size_t i = 0; i < m_vecModules.size(); ++i)
        {
            if (m_vecModules[i].iid.IsValid())
            {
                m_mapIndexByIid.insert(std::make_pair(m_vecModules[i].iid, i));
            }
            else
            {
                const char* strName = m_vecModules[i].module->GetName();
                if (strName != nullptr && strName[0] != '\0')
                {
                    m_mapIndexByName[strName] = i;
                }
            }
        }
    }
    pModule->Release();
    return true;
}

/// @brief 移除并释放所有模块。
void CModuleManager::Clear()
{
    std::vector<IModule*> vecToRelease;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        vecToRelease.reserve(m_vecModules.size());
        for (size_t i = 0; i < m_vecModules.size(); ++i)
        {
            vecToRelease.push_back(m_vecModules[i].module);
        }
        m_vecModules.clear();
        m_mapIndexByName.clear();
        m_mapIndexByIid.clear();
    }
    for (size_t i = 0; i < vecToRelease.size(); ++i)
    {
        vecToRelease[i]->Release();
    }
}

/// @brief 已注册模块数量。
size_t CModuleManager::Size() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_vecModules.size();
}

/// @brief 统一初始化所有模块。
///
/// 按注册顺序调用 Initialize；任一步失败时逆序关闭已初始化的模块。
///
/// @return true 全部初始化成功；false 存在失败并已回滚。
bool CModuleManager::InitializeAll()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // 构造初始化上下文（依赖注入）：模块在 Initialize(ctx) 中按类型解析依赖。
    CResolveContext context(*this);
    // 按依赖拓扑排序初始化（依赖在前）；无依赖声明时退化为注册顺序。
    std::vector<size_t> vecOrder = ComputeStartOrder();
    for (size_t k = 0; k < vecOrder.size(); ++k)
    {
        Entry& e = m_vecModules[vecOrder[k]];
        if (e.module->GetState() != ModuleState::kCreated)
        {
            continue; // 幂等：跳过非初始状态
        }
        if (!e.module->Initialize(context))
        {
            RollbackInitialized();
            return false;
        }
        SetModuleState(e.module, ModuleState::kInitialized);
    }
    return true;
}

/// @brief 统一启动所有模块。
///
/// 按注册顺序启动已初始化的模块；任一步失败时逆序停止并关闭已启动的模块。
///
/// @return true 全部启动成功；false 存在失败并已回滚。
bool CModuleManager::StartAll()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // 按依赖拓扑排序启动（依赖在前）；无依赖声明时退化为注册顺序。
    std::vector<size_t> vecOrder = ComputeStartOrder();
    for (size_t k = 0; k < vecOrder.size(); ++k)
    {
        Entry& e = m_vecModules[vecOrder[k]];
        if (e.module->GetState() != ModuleState::kInitialized)
        {
            continue; // 只启动已初始化的模块
        }
        if (!e.module->Start())
        {
            RollbackStarted();
            return false;
        }
        SetModuleState(e.module, ModuleState::kStarted);
    }
    return true;
}

/// @brief 统一停止所有模块（逆序）。
void CModuleManager::StopAll()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (size_t i = m_vecModules.size(); i > 0; --i)
    {
        Entry& e = m_vecModules[i - 1];
        if (e.module->GetState() == ModuleState::kStarted)
        {
            e.module->Stop();
            SetModuleState(e.module, ModuleState::kStopped);
        }
    }
}

/// @brief 统一关闭所有模块（逆序）。
void CModuleManager::ShutdownAll()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (size_t i = m_vecModules.size(); i > 0; --i)
    {
        Entry& e = m_vecModules[i - 1];
        if (e.module->GetState() != ModuleState::kCreated &&
            e.module->GetState() != ModuleState::kShutdown)
        {
            e.module->Shutdown();
            SetModuleState(e.module, ModuleState::kShutdown);
        }
    }
}

/// @brief 逆序关闭所有已初始化的模块（回滚辅助）。
void CModuleManager::RollbackInitialized()
{
    for (size_t i = m_vecModules.size(); i > 0; --i)
    {
        Entry& e = m_vecModules[i - 1];
        if (e.module->GetState() == ModuleState::kInitialized)
        {
            e.module->Shutdown();
            SetModuleState(e.module, ModuleState::kShutdown);
        }
    }
}

/// @brief 逆序停止并关闭已启动的模块（回滚辅助）。
void CModuleManager::RollbackStarted()
{
    // 先停止已启动的模块
    for (size_t i = m_vecModules.size(); i > 0; --i)
    {
        Entry& e = m_vecModules[i - 1];
        if (e.module->GetState() == ModuleState::kStarted)
        {
            e.module->Stop();
            SetModuleState(e.module, ModuleState::kStopped);
        }
    }
    // 再关闭已初始化的模块
    for (size_t i = m_vecModules.size(); i > 0; --i)
    {
        Entry& e = m_vecModules[i - 1];
        ModuleState state = e.module->GetState();
        if (state == ModuleState::kInitialized || state == ModuleState::kStopped)
        {
            e.module->Shutdown();
            SetModuleState(e.module, ModuleState::kShutdown);
        }
    }
}

/// @brief 模块是否存在（按名称）。
bool CModuleManager::HasModule(const char* strName) const
{
    return GetModule(strName) != nullptr;
}

/// @brief 模块是否存在（按接口标识）。
bool CModuleManager::HasModuleByIid(const InterfaceId& iid) const
{
    return GetModuleByIid(iid) != nullptr;
}

/// @brief 生成所有模块的结构化快照。
///
/// @return 快照列表（按注册顺序）。
std::vector<ModuleSnapshot> CModuleManager::Snapshot() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<ModuleSnapshot> vecSnapshot;
    vecSnapshot.reserve(m_vecModules.size());
    for (size_t i = 0; i < m_vecModules.size(); ++i)
    {
        ModuleSnapshot item;
        item.strIid = m_vecModules[i].iid.Name();
        const char* strName = m_vecModules[i].module->GetName();
        item.strName = (strName != nullptr) ? strName : "";
        item.state = m_vecModules[i].module->GetState();
        item.strStatus = m_vecModules[i].module->GetStatus();
        vecSnapshot.push_back(item);
    }
    return vecSnapshot;
}

/// @brief 生成所有模块的状态报告。
///
/// @return 多行状态文本，每行对应一个模块。
std::string CModuleManager::StatusReport() const
{
    std::vector<IModule*> vecModules;
    CollectModules(vecModules);
    std::string strReport;
    for (size_t i = 0; i < vecModules.size(); ++i)
    {
        if (!strReport.empty())
        {
            strReport += "\n";
        }
        strReport += vecModules[i]->GetStatus();
    }
    return strReport;
}

/// @brief 带超时地统一停止所有模块。
///
/// @param nTimeoutMs 总超时毫秒数。
///
/// @return true 表示在超时前完成；false 表示已超时（跳过剩余模块）。
bool CModuleManager::StopAllWithTimeout(uint32_t nTimeoutMs)
{
    std::vector<IModule*> vecModules;
    CollectModules(vecModules);
    std::chrono::steady_clock::time_point tpBegin = std::chrono::steady_clock::now();
    bool bComplete = true;
    for (size_t i = vecModules.size(); i > 0; --i)
    {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tpBegin).count() >= static_cast<int64_t>(nTimeoutMs))
        {
            bComplete = false;
            break;
        }
        vecModules[i - 1]->Stop();
    }
    return bComplete;
}

/// @brief 带超时地统一关闭所有模块。
///
/// @param nTimeoutMs 总超时毫秒数。
///
/// @return true 表示在超时前完成；false 表示已超时（跳过剩余模块）。
bool CModuleManager::ShutdownAllWithTimeout(uint32_t nTimeoutMs)
{
    std::vector<IModule*> vecModules;
    CollectModules(vecModules);
    std::chrono::steady_clock::time_point tpBegin = std::chrono::steady_clock::now();
    bool bComplete = true;
    for (size_t i = vecModules.size(); i > 0; --i)
    {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tpBegin).count() >= static_cast<int64_t>(nTimeoutMs))
        {
            bComplete = false;
            break;
        }
        vecModules[i - 1]->Shutdown();
    }
    return bComplete;
}

/// @brief 按依赖拓扑排序计算生命周期启动顺序。
///
/// 基于每个模块声明的接口依赖（CModule::GetDependencies），保证被依赖的
/// 接口模块先于依赖者；无依赖声明时退化为注册顺序（稳定）。
/// 依赖缺失或存在环时，相关模块追加到末尾，保证顺序确定。
///
/// @return 模块索引顺序（依赖在前）。
std::vector<size_t> CModuleManager::ComputeStartOrder() const
{
    const size_t n = m_vecModules.size();
    // 接口 IID → 提供该接口的模块索引（首个实例）
    std::map<InterfaceId, size_t> mapProvider;
    for (size_t i = 0; i < n; ++i)
    {
        if (m_vecModules[i].iid.IsValid() &&
            mapProvider.find(m_vecModules[i].iid) == mapProvider.end())
        {
            mapProvider[m_vecModules[i].iid] = i;
        }
    }
    // 建依赖图：入度 + 后继（依赖者）。
    std::vector<size_t> vecInDegree(n, 0);
    std::vector<std::vector<size_t> > vecSucc(n);
    for (size_t i = 0; i < n; ++i)
    {
        const std::vector<InterfaceId>& vecDeps = m_vecModules[i].module->GetDependencies();
        for (size_t k = 0; k < vecDeps.size(); ++k)
        {
            std::map<InterfaceId, size_t>::const_iterator it = mapProvider.find(vecDeps[k]);
            if (it == mapProvider.end() || it->second == i)
            {
                continue; // 依赖接口未注册或自依赖：忽略
            }
            vecSucc[it->second].push_back(i);
            ++vecInDegree[i];
        }
    }
    // Kahn 拓扑排序（稳定：同层保持注册顺序）。
    std::deque<size_t> queue;
    for (size_t i = 0; i < n; ++i)
    {
        if (vecInDegree[i] == 0)
        {
            queue.push_back(i);
        }
    }
    std::vector<size_t> vecOrder;
    vecOrder.reserve(n);
    std::vector<bool> vecVisited(n, false);
    while (!queue.empty())
    {
        size_t nCur = queue.front();
        queue.pop_front();
        vecOrder.push_back(nCur);
        vecVisited[nCur] = true;
        const std::vector<size_t>& vecNext = vecSucc[nCur];
        for (size_t k = 0; k < vecNext.size(); ++k)
        {
            size_t nV = vecNext[k];
            if (vecInDegree[nV] > 0)
            {
                --vecInDegree[nV];
                if (vecInDegree[nV] == 0)
                {
                    queue.push_back(nV);
                }
            }
        }
    }
    // 环 / 依赖缺失导致的剩余模块追加到末尾（保持注册顺序）。
    for (size_t i = 0; i < n; ++i)
    {
        if (!vecVisited[i])
        {
            vecOrder.push_back(i);
        }
    }
    return vecOrder;
}

/// @brief 收集所有已注册模块（锁外调用）。
///
/// @param vecOut 输出模块列表（按注册顺序）。
void CModuleManager::CollectModules(std::vector<IModule*>& vecOut) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    vecOut.reserve(m_vecModules.size());
    for (size_t i = 0; i < m_vecModules.size(); ++i)
    {
        vecOut.push_back(m_vecModules[i].module);
    }
}

} // namespace sc
