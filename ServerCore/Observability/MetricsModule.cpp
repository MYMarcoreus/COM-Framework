#include "Observability/MetricsModule.h"

#include "Module/InterfaceMap.h"
#include <cstdio>
#include <string>

namespace sc {

// 接口映射表：暴露本类实现的接口（查表驱动 QueryInterface）。
SC_DEFINE_INTERFACE_MAP(CMetricsModule, CModule, IMetrics)

/// @brief 创建指标注册表模块。
CMetricsModule::CMetricsModule() : CModule("metrics")
{
}

/// @brief 销毁指标注册表模块。
CMetricsModule::~CMetricsModule()
{
}

/// @brief 初始化模块（无配置依赖，直接成功）。
bool CMetricsModule::Initialize(const CResolveContext& /*ctx*/)
{
    return true;
}

/// @brief 模块启动（指标注册表无独立启动资源）。
bool CMetricsModule::Start()
{
    return true;
}

/// @brief 模块停止（指标数据保留，供状态报告）。
void CMetricsModule::Stop()
{
}

/// @brief 模块关闭（指标数据由析构释放）。
void CMetricsModule::Shutdown()
{
}

/// @brief 获取或创建指标条目（调用方持锁）。
///
/// 首次写入时按 kind 建立条目；后续同名写入保留首次类型（若冲突，计数器
/// 累加、仪表覆盖，均不会改变既有类型）。
CMetricsModule::Entry& CMetricsModule::EnsureEntry(const std::string& strName, MetricKind kind)
{
    std::map<std::string, Entry>::iterator it = m_mapMetrics.find(strName);
    if (it == m_mapMetrics.end())
    {
        Entry entry;
        entry.kind = kind;
        it = m_mapMetrics.insert(std::make_pair(strName, entry)).first;
    }
    return it->second;
}

/// @brief 计数器自增。
void CMetricsModule::Inc(const std::string& strName, double nDelta)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Entry& entry = EnsureEntry(strName, MetricKind::kCounter);
    entry.value += nDelta;
}

/// @brief 计数器自减。
void CMetricsModule::Dec(const std::string& strName, double nDelta)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Entry& entry = EnsureEntry(strName, MetricKind::kCounter);
    entry.value -= nDelta;
}

/// @brief 设置仪表值。
void CMetricsModule::SetGauge(const std::string& strName, double nValue)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Entry& entry = EnsureEntry(strName, MetricKind::kGauge);
    entry.value = nValue;
}

/// @brief 读取指定指标当前值。
double CMetricsModule::Get(const std::string& strName) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<std::string, Entry>::const_iterator it = m_mapMetrics.find(strName);
    return (it != m_mapMetrics.end()) ? it->second.value : 0.0;
}

/// @brief 生成全量指标快照（按名称排序）。
std::vector<MetricSnapshot> CMetricsModule::Snapshot() const
{
    std::vector<MetricSnapshot> vecResult;
    std::lock_guard<std::mutex> lock(m_mutex);
    vecResult.reserve(m_mapMetrics.size());
    for (std::map<std::string, Entry>::const_iterator it = m_mapMetrics.begin();
         it != m_mapMetrics.end(); ++it)
    {
        vecResult.push_back(MetricSnapshot(it->first, it->second.kind, it->second.value));
    }
    return vecResult;
}

/// @brief 已注册指标数量。
size_t CMetricsModule::Count() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_mapMetrics.size();
}

/// @brief 状态报告：列出全部指标。
std::string CMetricsModule::GetStatus() const
{
    std::string strStatus = "metrics:";
    std::vector<MetricSnapshot> vecSnapshot = Snapshot();
    for (size_t i = 0; i < vecSnapshot.size(); ++i)
    {
        strStatus += (i > 0 ? " " : "");
        char szValue[32];
        std::snprintf(szValue, sizeof(szValue), "%.0f", vecSnapshot[i].value);
        strStatus += vecSnapshot[i].strName + "=" + szValue;
    }
    return strStatus;
}

} // namespace sc
