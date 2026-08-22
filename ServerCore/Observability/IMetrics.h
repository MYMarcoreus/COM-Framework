#pragma once

#include <string>
#include <vector>

#include "Module/IUnknown.h"
#include "Module/InterfaceDecl.h"

namespace sc {

/// @brief 指标类型。
enum class MetricKind
{
    kCounter, // 计数器：只增不减（累计值，如收包数 / 连接数累计）
    kGauge    // 仪表：可增可减（当前值，如活跃连接数）
};

/// @brief 指标快照（供状态报告 / 健康检查）。
struct MetricSnapshot
{
    std::string strName; // 指标名（点分，如 "network.conns"）
    MetricKind kind;     // 指标类型
    double value;        // 当前值

    MetricSnapshot() : kind(MetricKind::kCounter), value(0.0) {}

    MetricSnapshot(const std::string& name, MetricKind metricKind, double metricValue)
        : strName(name), kind(metricKind), value(metricValue) {}
};

/// @brief 指标注册表接口（COM 风格：继承 IUnknown）。
///
/// 统一可观测性基础设施：模块注册命名指标（计数器 / 仪表），
/// 应用层聚合为状态报告 / 健康检查，替代手拼字符串的状态描述。
SC_INTERFACE(IMetrics, "sc::IMetrics", "9c1b3d42-7a51-4f6e-9a2b-cd8e55f0a3f1")
{
public:
    virtual ~IMetrics() {}

    // 计数器自增（默认 +1）。
    virtual void Inc(const std::string& strName, double nDelta = 1.0) = 0;

    // 计数器自减（累计值回退，慎用）。
    virtual void Dec(const std::string& strName, double nDelta = 1.0) = 0;

    // 设置仪表值（覆盖当前值）。
    virtual void SetGauge(const std::string& strName, double nValue) = 0;

    // 读取指定指标当前值；不存在返回 0。
    virtual double Get(const std::string& strName) const = 0;

    // 生成全量指标快照（按名称排序）。
    virtual std::vector<MetricSnapshot> Snapshot() const = 0;

    // 已注册指标数量。
    virtual size_t Count() const = 0;
};

} // namespace sc
