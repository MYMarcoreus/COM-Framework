#include "Module/Module.h"

#include <string>

namespace sc {

/// @brief 创建模块。
///
/// 初始引用计数为 1。
///
/// @param strName 模块名称（进程内唯一，用于管理与日志；可为空）。
CModule::CModule(const char* strName)
    : m_nRefCount(1), m_strName(strName != nullptr ? strName : "")
{
}

/// @brief 销毁模块。
CModule::~CModule()
{
}

/// @brief 增加引用计数。
///
/// @return 增加后的引用计数。
unsigned int CModule::AddRef()
{
    return m_nRefCount.fetch_add(1) + 1;
}

/// @brief 减少引用计数。
///
/// 引用计数归零时销毁模块。
///
/// @return 减少后的引用计数。
unsigned int CModule::Release()
{
    unsigned int nCount = m_nRefCount.fetch_sub(1) - 1;
    if (nCount == 0)
    {
        delete this;
    }
    return nCount;
}

/// @brief 查询接口。
///
/// 优先匹配 IUnknown，其余接口交给子类的 QueryInterfaceImpl。
///
/// @param iid 接口标识。
/// @param ppv 输出接口指针。
///
/// @return true 查询成功；false 未找到接口。
bool CModule::QueryInterface(const InterfaceId& iid, void** ppv)
{
    if (ppv == nullptr)
    {
        return false;
    }
    *ppv = nullptr;
    if (iid == nullptr)
    {
        return false;
    }
    if (std::string(iid) == std::string(IID_IUnknown()))
    {
        *ppv = static_cast<IUnknown*>(this);
        return true;
    }
    return QueryInterfaceImpl(iid, ppv);
}

/// @brief 获取模块名称。
const char* CModule::GetName() const
{
    return m_strName.c_str();
}

/// @brief 默认状态报告。
///
/// @note 默认返回模块名称，子类按需重写以提供更有意义的状态。
std::string CModule::GetStatus() const
{
    return m_strName.empty() ? "module" : m_strName;
}

/// @brief 默认初始化实现，子类按需重写。
///
/// @return true。
bool CModule::Initialize()
{
    return true;
}

/// @brief 默认启动实现，子类按需重写。
///
/// @return true。
bool CModule::Start()
{
    return true;
}

/// @brief 默认停止实现，子类按需重写。
void CModule::Stop()
{
}

/// @brief 默认关闭实现，子类按需重写。
void CModule::Shutdown()
{
}

/// @brief 接口查询实现。
///
/// 暴露 IModule 接口，其余接口交给子类继续分发。
///
/// @note 接口标识使用字符串内容比较（跨翻译单元地址不可靠，与现有模块实现一致）。
bool CModule::QueryInterfaceImpl(const InterfaceId& iid, void** ppv)
{
    if (std::string(iid) == std::string(IID_IModule()))
    {
        *ppv = static_cast<IModule*>(this);
        return true;
    }
    return false;
}

} // namespace sc
