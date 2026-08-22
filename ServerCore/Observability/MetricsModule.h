#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "Observability/IMetrics.h"
#include "Module/InterfaceMap.h"
#include "Module/Module.h"

namespace sc {

/// @brief 指标注册表模块。
///
/// 线程安全：Inc / Dec / SetGauge / Snapshot 均可跨线程调用。
/// 内部按名称维护原子值，首次写入自动注册（类型以首次写入为准）。
class CMetricsModule : public CModule, public IMetrics
{
public:
    CMetricsModule();

    virtual ~CMetricsModule();

    // 生命周期：无资源需管理，直接成功 / 空操作。
    bool Initialize(const CResolveContext& ctx) override;
    bool Start() override;
    void Stop() override;
    void Shutdown() override;

    // 计数器自增。
    void Inc(const std::string& strName, double nDelta = 1.0) override;

    // 计数器自减。
    void Dec(const std::string& strName, double nDelta = 1.0) override;

    // 设置仪表值。
    void SetGauge(const std::string& strName, double nValue) override;

    // 读取指定指标当前值。
    double Get(const std::string& strName) const override;

    // 生成全量指标快照。
    std::vector<MetricSnapshot> Snapshot() const override;

    // 已注册指标数量。
    size_t Count() const override;

    // 状态报告：列出全部指标。
    std::string GetStatus() const override;

    SC_DECLARE_INTERFACE_MAP();

private:
    struct Entry
    {
        MetricKind kind;      // 指标类型（首次写入时确定）
        mutable double value; // 当前值（读写受 m_mutex 保护）

        Entry() : kind(MetricKind::kCounter), value(0.0) {}
    };

    // 获取或创建指标条目（锁外调用）。
    Entry& EnsureEntry(const std::string& strName, MetricKind kind);

    std::map<std::string, Entry> m_mapMetrics;
    mutable std::mutex m_mutex;
};

} // namespace sc
