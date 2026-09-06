# DataHub 存储与状态模型

> 数据面全部数据在**进程内内存**。本文讲清三套易混的内存模型、它们的关系、数据隔离与
> 删除级联、配额、以及重启语义。通用概念（共享表/隔离维度）见 `tenant-concepts.md`；
> 租户落地决策见 `tenant-project.md`。

## 1. 三套内存模型（别混淆）

| # | 模型 | 持有者 | 内容 | 生命周期 |
|---|---|---|---|---|
| ① | **租户与花名册** | `CTenantModule` | 租户实体（码/名/配额/创建时间）+ 每租户成员册（account→角色/加入时间） | 模块进程内存 |
| ② | **数据项** | `DataStoreModule` → Common `CFileStore` | 各租户的消息/文件（共享表 + 行级 tenant） | 模块进程内存 |
| ③ | **在线成员** | `CMemberService` | 按租户的“此刻活跃”设备（ip/首次/最后活跃） | 30s TTL 被动清理 |

- ① 是**正式成员关系**（决定能否进租户、能否删别人的数据，Owner/Member）。
- ② 是**业务数据本体**（聊天的文本/文件）。
- ③ 是**在线指示器**（仅展示“谁在线”，30s 无活跃即消失）。

> 关键词防混淆：`/api/tenant/members` 读 ①（花名册，需为成员/owner 可见）；
> `/api/members` 读 ③（在线活跃）。删除授权用的是 ① 的角色 + ② 的 `strFrom`。

## 2. 数据结构速览

```cpp
// ① CTenantModule（sc::ITenantService）
std::map<std::string, CTenant> m_mapTenants;                          // [码]→实体
std::map<std::string,
         std::map<std::string, CTenantMember>> m_mapMembers;          // [租户码][账号]
struct CTenantLimits { std::size_t nMaxItems; std::uint64_t nMaxTotalBytes; std::uint64_t nMaxItemBytes; }; // 0=不限
struct CTenant { std::string strCode; std::string strName; CTenantLimits limits; std::int64_t nCreateMs; };
enum class TenantRole : int { kOwner = 0, kMember = 1 };
struct CTenantMember { std::string strAccountId; TenantRole role; std::int64_t nJoinMs; };

// ② CFileStore（DataStoreModule 内部封装），每行一条数据，带 strTenant
struct DataItemInfo { std::string strId; DataKind kind; std::string strName;
                      std::string strFrom; std::uint64_t nSize;
                      std::int64_t nCreateMs; std::uint64_t nSeq; };  // kText=0 / kFile=1
std::map<std::string, DataItemInfo> m_mapItems;                       // [id]→条目（共享表）
std::map<std::string, PerTenantStats> m_mapStats;                     // 每租户统计

// ③ CMemberService
std::map<std::string, std::map<std::string, MemberInfo>> m_mapByTenant; // [租户码][clientId]
struct MemberInfo { std::string strIp; std::int64_t nFirstMs; std::int64_t nLastMs; };
```

## 3. 数据隔离：共享表 + 行级 tenant，逻辑隔离

- 数据面**不按租户分进程/分库**：`CFileStore` 是**一张共享表**，每行数据带 `strTenant`，
  每租户独立统计桶与配额。隔离是**应用层逻辑隔离**（存储接口全部以 `const CTenant&` 为边界，
  `DataStoreModule` 把租户转成 `strCode` 后访问对应桶）。
- **物理隔离发生在进程级**：数据面(8888) 与控制面(8899) 是两个 exe；设备域只能到达数据面，
  管理域只走控制面回环代理——这是“边界”在物理上的落点。
- 业务 API 必须显式 `X-Tenant`（B4）→ `OnRequest` `FindTenant` → `ctx.tenant` 作为存储调用边界；
  未知租户一律 404（`/api/tenant/state` 例外，返回 exists:false 供对账）。

### 租户码 / 数据 id（短码）

- 租户码：6 位，字符集去掉易混淆的 `0/O/1/I/l`（`23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz`），
  生成冲突则重抽；`public` 保留给公共租户。
- 数据 id：同风格短码，由 `CFileStore` 生成；`seq` 为全表单调递增序号（增量游标）。

## 4. 配额（CTenantLimits）如何生效

```text
datahub.ini [store]  →  CTenantModule 默认 CTenantLimits（0=不限）
      ↓ CreateTenant 新建租户继承默认配额，存于实体 limits
DataStoreModule::LimitsOf(CTenant) 把 limits 映射为 StoreLimits
      ↓
CFileStore::SaveText/SaveFile 按 StoreLimits 执行：条数 / 总字节 / 单条 任一超限 → 保存失败
```

要点：**CTenantModule 只“携带”配额，不自行限流**；真正的写入限额在存储层转译执行
（`DataStoreModule` 把 `CTenant` 变成 `StoreLimits` 传给通用 `CFileStore`）。这样
`CFileStore` 仍是通用组件（不懂租户语义），限额/隔离逻辑都在适配层完成。

## 5. 删除级联（谁负责删什么）

| 动作 | 谁做 | 结果 |
|---|---|---|
| `DELETE /api/admin/tenant?code=` | `CAdminController`：先 `RemoveTenant`（删实体+花名册）再 `IDataStore::PurgeTenant` | 租户、花名册、数据项全清；`public` 拒绝 |
| `RemoveMember` / `SetMemberRole` | `CTenantModule`，带 **OwnerCountLocked** 守卫 | 不能移除/降级**最后一个 Owner**（租户不可“无主”） |
| `DELETE /api/admin/item` / `DELETE /api/item/{id}` | Admin：直接删；设备侧：Owner 任意、Member 自删（`strFrom` 判定），否则 403 | 单条数据删除 |

> 设计取舍：`RemoveTenant` 不连带删数据（接口只动租户域），数据清理由调用方显式
> `PurgeTenant` —— 保持租户服务与存储解耦（见 `CTenantModule.cpp` 注释）。

## 6. 在线成员（③ CMemberService）细节

- 记录条件：请求**携带非空 `X-Client-Id`** 才 Touch（无头请求/静态/探测不计入，
  避免 `IP:port` 假成员）；来源 IP = `req.Peer()` 去端口；每租户按 clientId 记
  `{ip, first, last}`。
- TTL：`nTimeoutMs = 30000`（30s）。无后台定时器——**被动清理**：`GET /api/members`
  (`HandleMembers`) 先 `Prune()`（顺带删空租户桶）再快照。
- 线程：单把 `mutex`；Workflow 请求回调里调用，无独立线程。

## 7. 设备凭据表（CDeviceRegistry）

独立于以上三套（它属于“认证”，不是业务数据）：

```cpp
std::map<std::string, std::string> m_mapToken;   // clientId → 随机令牌（32 hex，128 位熵）
```

- `Register` 幂等：已注册返回既有令牌，否则生成新令牌；`Verify` 常数时间比较。
- **纯内存**：服务重启即清空 —— 这正是 B6 的成因之一；靠前端“幂等重注册 + 401 自动续期”自愈
  （见 `datahub-frontend.md` §对账，`tenant-audit.md` B1/B6）。

## 8. 重启语义（纯内存的代价与对策）

| 场景 | 现象 | 对策 |
|---|---|---|
| 服务重启 | 全部租户/数据/花名册/令牌清空（只剩 `public`） | 文档注明：本版为**演示级纯内存**；扩展落盘可换 `IDataStore` 实现 |
| 客户端 localStorage 旧租户 | 进入后 `tenant not found` 404 循环 | `/api/tenant/state` 对账 → 自动移出 + 回落 public（B2/B6） |
| 客户端旧令牌 | 业务请求 401 | 幂等重注册续新令牌 + 401 自动重试一次（B1/B6） |
| 控制面重启 | 无业务数据，无需恢复 | 上游 base/token 来自 `datahub-admin.ini` |

> 想持久化：① 给 `IDataStore` 实现落盘/DB 后端（数据项）；② 租户/花名册/令牌若需跨重启，
> 同样需要持久层或引入外部状态（如 SQLite/Redis）。当前版本有意保持“零依赖、纯内存”。

## 9. 相关文档

- 存储模型概念对照（三类型/共享表）：`tenant-concepts.md` §6
- 本项目共享表落地/实体/分层：`tenant-project.md`
- 存储与接口签名：`IDataStore.h`、`ITenantService.h`、`Common/Storage/CFileStore.*`
