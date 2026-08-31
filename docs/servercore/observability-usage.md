# 可观测性 — 使用文档

> 实现细节见：[observability-impl.md](observability-impl.md)

## 1. 接口（IMetrics）

统一指标基础设施：模块注册命名指标（计数器 / 仪表），应用层聚合为状态报告，替代手拼字符串的状态描述。

```cpp
class IMetrics : public virtual IUnknown
{
    virtual void Inc(const std::string& strName, double nDelta = 1.0) = 0;  // 计数器自增
    virtual void Dec(const std::string& strName, double nDelta = 1.0) = 0;  // 计数器自减
    virtual void SetGauge(const std::string& strName, double nValue) = 0;   // 仪表值（覆盖）
    virtual double Get(const std::string& strName) const = 0;
    virtual std::vector<MetricSnapshot> Snapshot() const = 0;   // 全量快照（按名称排序）
    virtual size_t Count() const = 0;
};
```

`MetricSnapshot` = `{ strName, kind(kCounter|kGauge), value }`。

## 2. 使用

`CMetricsModule` 已由 `CMyApplication` 默认装配（`IID_IMetrics()`）。模块在 `Initialize(ctx)` 解析并上报：

```cpp
bool CMyModule::Initialize(const sc::CResolveContext& ctx)
{
    m_pMetrics.Reset(ctx.Resolve<sc::IMetrics>());   // 可选依赖
    return true;
}
void CMyModule::OnData(...)
{
    if (m_pMetrics != nullptr) { m_pMetrics->Inc("my.module.msgs"); }
}
```

## 3. 内置指标（网络模块）

| 指标 | 类型 | 含义 |
|---|---|---|
| `network.accepted` | counter | 累计接受连接数 |
| `network.closed` | counter | 累计关闭连接数 |
| `network.conns` | gauge | 当前活跃连接数 |
| `network.msgs` | counter | 累计接收消息数 |

## 4. 状态报告

`CMyApplication` 收到 `SIGUSR1` 时输出状态报告，含两部分：模块状态（每行 `GetStatus()`）+ `[metrics]` 指标段。
也可编程查询：`app.MetricsReport()` 返回指标聚合文本。

## 5. 命名约定

指标名使用点分层次：`<模块>.<量名>`（如 `network.conns`、`example.echo`）；计数器只增；仪表表示当前值（可增可减）。
