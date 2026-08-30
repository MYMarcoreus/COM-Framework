# 可观测性 — 实现文档

> 配套使用文档：[observability-usage.md](observability-usage.md)
> 源码：`ServerCore/Observability/MetricsModule.*`

## 1. 数据结构

```cpp
std::map<std::string, Entry> m_mapMetrics;   // 名称 → {MetricKind kind; double value;}
mutable std::mutex m_mutex;                  // 单把锁保护（非原子、非双缓冲）
```

`std::map` 天然按名称**字典序**排序，`Snapshot()` 无需额外排序即可按序输出。

## 2. 关键操作

- `EnsureEntry`：首次写入定类型（同名后续写入**不改变类型**——计数器不会变成仪表）；
- `Inc / Dec / SetGauge / Get / Count`：均 `lock_guard(m_mutex)`；
- `Snapshot`：持锁遍历 map 生成 `MetricSnapshot(name, kind, value)` 向量；
- `GetStatus`：用 `%.0f` 拼 `metrics:name=value ...` 文本。

## 3. 线程安全

所有指标读写都在同一把 `m_mutex` 下串行化，可跨线程安全调用（网络线程上报、业务线程上报互不冲突）；
代价是高频路径（如 `network.msgs` 每消息 `Inc`）有锁开销——指标量小、低频可接受。

## 4. 与状态报告

`CMyApplication` 在 `SIGUSR1` 时调用各模块 `GetStatus()`（含 `CMetricsModule` 的聚合文本）拼出
`[Status]` + `[metrics]` 两段报告；`MetricsReport()` 返回该聚合文本供编程查询。
