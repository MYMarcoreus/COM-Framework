#include "Module/ModuleManager.h"

#include <chrono>

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

/// @brief 注册模块。
///
/// 校验模块非空、名称非空且不重复；成功后持有模块一个引用。
///
/// @param module 模块接口指针。
///
/// @return true 注册成功；false 参数非法或名称重复。
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
        return false;
    }
    if (m_mapIndexByName.find(strName) != m_mapIndexByName.end())
    {
        return false; // 名称重复
    }
    pModule->AddRef();
    m_mapIndexByName[strName] = m_vecModules.size();
    m_vecModules.push_back(Entry(pModule));
    return true;
}

/// @brief 按接口标识注册模块。
///
/// 用于服务定位（如按 INetwork 获取网络模块）；同一接口标识只能注册一次。
/// 注册成功后管理器持有模块的一个引用。
///
/// @param iid 接口标识。
/// @param module 模块接口指针。
///
/// @return true 注册成功；false 参数非法或接口标识重复。
bool CModuleManager::RegisterModule(const InterfaceId& iid, IModule* pModule)
{
    if (iid == nullptr || pModule == nullptr)
    {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::string strKey(iid);
    if (m_mapIndexByIid.find(strKey) != m_mapIndexByIid.end())
    {
        return false; // 接口标识重复
    }
    pModule->AddRef();
    m_mapIndexByIid[strKey] = m_vecModules.size();
    m_vecModules.push_back(Entry(pModule, strKey));
    return true;
}

/// @brief 接管型注册（按名字）。
///
/// 直接接管调用方 new 出来的模块的创建者引用（不额外 AddRef）；
/// 成功时管理器持有该引用（Clear/Unregister 时 Release 归零析构），
/// 失败（名字为空/重复）时管理器负责 Release 并返回 false，调用方无需释放。
///
/// @param module 刚创建、尚未额外 AddRef 的模块接口指针。
///
/// @return true 注册成功并接管；false 注册失败（已释放）。
bool CModuleManager::RegisterModuleOwned(IModule* pModule)
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

/// @brief 接管型注册（按接口标识）。
///
/// 语义与按名字版本一致：接管创建者引用，失败时自行 Release。
///
/// @param iid 接口标识。
/// @param module 刚创建、尚未额外 AddRef 的模块接口指针。
///
/// @return true 注册成功并接管；false 注册失败（已释放）。
bool CModuleManager::RegisterModuleOwned(const InterfaceId& iid, IModule* pModule)
{
    if (iid == nullptr || pModule == nullptr)
    {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::string strKey(iid);
    if (m_mapIndexByIid.find(strKey) != m_mapIndexByIid.end())
    {
        pModule->Release(); // 接口标识重复：释放
        return false;
    }
    m_mapIndexByIid[strKey] = m_vecModules.size();
    m_vecModules.push_back(Entry(pModule, strKey)); // 接管引用（不 AddRef）
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
/// @return 借用指针（不增加引用计数）；未找到返回 nullptr。
IModule* CModuleManager::GetModuleByIid(const InterfaceId& iid) const
{
    if (iid == nullptr)
    {
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::map<std::string, size_t>::const_iterator it = m_mapIndexByIid.find(std::string(iid));
    if (it == m_mapIndexByIid.end())
    {
        return nullptr;
    }
    return m_vecModules[it->second].module;
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
            if (!m_vecModules[i].strIid.empty())
            {
                m_mapIndexByIid[m_vecModules[i].strIid] = i;
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
    if (iid == nullptr)
    {
        return false;
    }
    IModule* pModule = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        std::map<std::string, size_t>::iterator it = m_mapIndexByIid.find(std::string(iid));
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
            if (!m_vecModules[i].strIid.empty())
            {
                m_mapIndexByIid[m_vecModules[i].strIid] = i;
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
    for (size_t i = 0; i < m_vecModules.size(); ++i)
    {
        Entry& e = m_vecModules[i];
        if (e.module->GetState() != ModuleState::kCreated)
        {
            continue; // 幂等：跳过非初始状态
        }
        if (!e.module->Initialize())
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
    for (size_t i = 0; i < m_vecModules.size(); ++i)
    {
        Entry& e = m_vecModules[i];
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
        item.strIid = m_vecModules[i].strIid;
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
