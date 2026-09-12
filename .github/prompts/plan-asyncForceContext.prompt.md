# 计划：把「可选 / 懒创建 / 无效」态改成编译期强制（精简异步框架）

> **诉求原话**：「Context 不要搞得太麻烦了，应该强制用户传入；对于其他更多的地方，我们是否能做到强制用户这么使用，从而达到精简代码的目的呢？」
> **基线**：分支 `feature/async-js`，HEAD = `ad37843`（测试 160/160，异步 114）。
> **当前工作区**：批次 1 的源码改动**已落盘但未提交**（3 个文件，未编译验证）。
> **相关技能**：`cpp-development`、`documentation`、`name-standard`、`server-core`。

---

## 0. 判断标准（先说清楚"什么才算精简"）

**能用「编译不过」表达的约束，就不要写成运行时的分支持久态。**

框架里现有的三种「可选态」形式，以及强制化之后的收益：

| 形式 | 现状例子 | 强制之后能删掉什么 |
| --- | --- | --- |
| **懒创建**（传空 → 首次使用时造） | 共享上下文 `CPromiseCore::Context()` | 保护锁、`m_spLazyContext`、`LazyContext()`、每层一次空判 |
| **无效态**（默认构造 → 处处空操作） | 无效 promise（`m_pCore == nullptr`） | `IsValid()`、2 条诊断文案、`OnSettled` 的 `bool` 返回、13 处空判 |
| **双形态**（同一条链两种启动模式） | `BuildPromise` 延迟启动 | `m_bDeferred` 原子、`m_pLaunch`、`IsDeferred/IsStarted/Start`、`Append`/`ThenPromise` 的首层与延迟分支 |

**收益口径**（写进提交信息时用同一个尺子）：

- 删掉的**代码行 + Doxygen 行**；
- 少掉的**分支**（尤其"每层都会走一次"的热路径分支）；
- 少掉的**共享可变状态**（锁、原子、可空指针）；
- **不**把"用户少写一行"算作收益（那是 API 便利性，与精简无关，两者冲突时以「少状态」优先）。

---

## 1. 批次 1：共享上下文强制传入（**进行中**，低风险）

### 1.1 已完成（工作区未提交）

| 文件:行 | 改动 | 说明 |
| --- | --- | --- |
| `Common/Async/Promise.h:507-525` | `CPromiseCore` 删 `mutable std::mutex m_mutex` / `m_spLazyContext` / `LazyContext()`；构造初始化列表同步 | 核心**再无可变共享状态** |
| `Common/Async/Promise.h:520-524` | `Context()` 改为 `const std::shared_ptr<TContext>& Context() const { return m_spContext; }` | 恒非空 → 不加锁、不拷贝 `shared_ptr`（每层 1–2 次调用） |
| `Common/Async/Promise.h:1022-1025` | `CPromise::GetContext()` 注释改为「恒非空，由调用方强制传入」 | 行为不变（仍返回值） |
| `Common/Async/AsyncExecutor.h:239` | `BuildPromise(spCtx)` 去掉默认实参 | 不传 ctx → **编译不过** |
| `Common/Coroutine/Coroutine.h:112-113` | `explicit CCoroutine(spContext)` 去掉默认实参 + 注释 | 协程同上 |

### 1.2 待办（做完才能编译通过）

| # | 文件:行 | 动作 |
| --- | --- | --- |
| 1 | `Tests/test_async_chain.cpp:216-229` | `TEST(Promise_LazyContext)` 用了 `BuildPromise<CTestContext>()` → 改名为 `Promise_BuildThenFillContext`：显式 `make_shared` → `BuildPromise(spCtx)` → **先填初始数据再挂层**（保留原有覆盖价值：建链与挂层之间可改上下文） |
| 2 | `examples/main.cpp:445-466` | demo ⑨ 去掉 9.1「链内懒创建」；改成「显式传入（唯一方式）：先备好数据再挂层、再 Start」，9.2 保持；打印文案去掉"懒创建" |
| 3 | `Common/Async/Promise.h:1440` | `BuildPromise` 定义处 `@param spContext（所有层共用；可为空 → 首次取用时懒创建）` → `（必传；所有层共用同一实例）` |
| 4 | `Common/Async/AsyncExecutor.h:589/610/631/649` | 组合器四处 `@param spContext ...（可为空 → 首次取用时懒创建）` → `（必传）` |
| 5 | `Common/Async/Promise.h:640` 附近（文件头对照表） | `exec.CoStart<T>()` → `exec.CoStart<T>(spCtx)` |
| 6 | `docs/common/async-usage.md:147-158` | 删「方式 B：promise 内部懒创建」，只留「方式 A：外部准备数据后注入」；`TContext` 必须可默认构造的约束整条删除；补一句「**框架不做懒创建**：谁起链谁备上下文」 |
| 7 | `docs/common/async-usage.md:600` 附近 | `common::async::CPromise<Ctx> c1 = exec.BuildPromise<Ctx>();` → 传 `spCtx` |
| 8 | `docs/common/async-impl.md:489`（§12 性能账本） | 「锁只留给『未传 ctx、懒创建』的冷路径（`m_spLazyContext`）」→ 改为「**核心已无锁**：上下文强制传入、构造后只读」 |
| 9 | `docs/common/async-impl.md` §4/§5 若有「懒创建」字样 | 全文 grep `懒创建` 清零 |
| 10 | `docs/common/coroutine-usage.md` / `coroutine-impl.md` | `CCoroutine` 构造与 `CoStart` 的「可为空」说法改为「必传」 |

### 1.3 影响面与风险

- 编译期影响：`Tests` / `examples` 各 1 处（1.2 的 #1 #2），无其他调用点（全仓 `BuildPromise` 无参调用只有这两处）。
- 行为变更：`BuildPromise<T>()` 从「合法」变「编译不过」；`GetContext()` 在有效句柄上语义不变。
- 风险：低。唯一需要留意的是 `Context()` 现在返回**引用**（detail 内部使用）——所有调用点都在 detail 内、且立即用于构造任务体或返回值拷贝，不存在"存下引用后对象析构"的路径（`CCoroutine::GetContext()` / `CPromise::GetContext()` 都返回值）。
  - 若要更保守：`Context()` 返回值（性能略差、语义更安全）。**建议保留引用版**，因为它正好等于「恒非空」这一事实的表达式。
- 性能：`CPromiseCore` 每链少 1 个 `mutex`(40B)+`shared_ptr`(16B)；每层少 1 次 `shared_ptr` 拷贝（2 次原子操作）。分配预算断言（`Tests/test_async_alloc.cpp`）**不变**（= 2 次/层建链、1 次/层跑链），只是字节数略降 → 若打印值变化需在提交信息里解释。

### 1.4 提交信息模板

```
refactor(async): 共享上下文改为强制传入（删懒创建 + 去掉核心的锁）

- CPromiseCore：删 m_spLazyContext / LazyContext() / m_mutex —— 核心再无可变共享状态
- Context() 改为返回 const shared_ptr&（恒非空，构造后只读）→ 热路径不加锁、不拷贝
- BuildPromise(spCtx) / CCoroutine(spCtx) 去掉默认实参：不传上下文直接编译不过
- 调用点与文档同步（examples ⑨、Tests Promise_LazyContext → Promise_BuildThenFillContext、
  async-usage §5 只留「外部注入」、async-impl §12 改写）

构建 0 warning；tests 160/160（debug + release）；TSan 0 竞争；examples 通过。
```

---

## 2. 批次 2：promise 恒有效（删掉「无效句柄」这一态）—— **破坏性，需拍板**

### 2.1 能删什么（当前行号）

| 位置 | 现状 | 强制化后 |
| --- | --- | --- |
| `Promise.h:150-155` | `kDiagInvalidLayer` / `kDiagInvalidAdopt` 两条诊断文案 | **删**（误用变编译错误） |
| `Promise.h:668-671` | `IsValid()` | **删**（或仅留内部私有检查） |
| `Promise.h:821-825` | `ThenPromise` 的 `m_pCore == nullptr` 分支 | 删（-5 行） |
| `Promise.h:932-941` / `963-972` | `OnSettled` / `OnSettledOn` 的 `m_pCore == nullptr` 检查 + `bool` 返回 | **返回类型改 `void`**，删检查（-8 行，且调用方不再可能忽略"注册失败"） |
| `Promise.h:1028-1032` | `GetContext()` 空 core 返回空指针 | 删（-5 行） |
| `Promise.h:1067` | `ReportBlockingRisk` 里的空 core 判 | 删 |
| `Promise.h:1297-1301` | `Append` 的无效 promise 诊断分支 | 删（-5 行） |
| `AsyncExecutor.h:459-464` | `BindChildGather` 的 `!promiseChild.IsValid()` → `kStopped` | 删（-5 行） |
| `Promise.h:1230` | `Adopt` 的 `!promiseChild.IsValid()` → `kStopped` | 删（-5 行） |
| `Promise.h:905-908` | `ThenBridge` 里 `if (!promiseChild.IsValid()) return CPromise();` | 删（-5 行） |

**合计净删约 45–55 行代码 + 2 条诊断文案 + 1 个测试用例 + 2 个测试用例改写。**

### 2.2 需要接受的行为变化

1. `CPromise<T>()` 默认构造**取消** → 「先声明后赋值」不再可能；
2. 由 1 推出：**没有"无效句柄"这种对象**，因此"A 无效句柄上挂层会静默不跑"这一整类坑在编译期消失；
3. `OnSettled` 不再返回 `bool`（唯一 `false` 情形消失）→ 调用方写法更简单；
4. `fnCreate` / promise 工厂**必须返回有效子链**（不再能返回"空"来表示"没有子链"）→ `ThenBridge` 的语义收紧为"工厂必须给出子链"。

### 2.3 需要改写的用户代码（已清点）

| 文件:行 | 现写法 | 迁移写法 |
| --- | --- | --- |
| `examples/main.cpp:551` | `CPromise<CDemoContext> tail;`（跨作用域保活） | `std::unique_ptr<CPromise<CDemoContext>> tail;` + `*tail = exec.NewPromise(...)`（或把 Await 移进作用域并改演示点） |
| `examples/main.cpp:1015-1017` | 三个成员：`m_head` / `m_branchA` / `m_branchB` | 成员改为 `std::shared_ptr<CPromise<CDemoContext>>`，或用 `exec.BuildPromise(spCtx)` 初始化（需要 exec 先就绪：把成员初始化挪到 `Start()`） |
| `Tests/test_async_chain.cpp:506` | `tail`（同 examples:551） | 同上（unique_ptr） |
| `Tests/test_async_affinity.cpp:158` | 成员 `m_pStock` | 改 `std::shared_ptr<CPromise<CCalleeCtx>>` 或改成局部变量（看用例结构） |
| `Tests/test_async_combine.cpp:187` | 专测"无效子 promise 计入 kStopped" | **用例删除**（能力不存在了） |
| `Tests/test_async_settled_delivery.cpp:264` | `SettledNotice_InvalidPromiseReturnsFalse` | **用例删除** |
| `Tests/test_async_robustness.cpp:305-339` | `Robust_InvalidPromiseAppendReports` | **用例删除**，其"误用提示"由编译期承担 |

文档同步：`async-impl.md:237/252/384`、`async-usage.md:259/440`、`async-cross-module-findings.md:237`（"唯一仍返回 false 的情形"整段）、`servercore/testing-usage.md` 的用例清单、`docs/common/*` 中 grep `无效 promise` / `IsValid`。

### 2.4 验证与风险

- 风险：**中高**（公开 API 破坏 + 3 个用例删除 + 5 处用户代码改写）。收益也最大（删掉整个"无效态"语义）。
- 测试计数会从 160 降到约 157（删 3 例）→ 文档里的计数（`async-usage.md` §14、`async-impl.md` §11、`testing-usage.md`）必须同步。
- 务必确认：`CPromise::Make`（协程 `AsPromise`）、`NewLayer`、`Adopt`、`Gather` 内部的默认构造实例全部改成私有构造路径（否则编译不过，属正常反馈）。

---

## 3. 批次 3：延迟启动独立成类型（删掉链上的"双形态"）—— **结构收益 > 行数收益，可选**

### 3.1 问题

`BuildPromise` 让**同一个 `CPromise` 类型**同时承担两种启动模式，于是链上到处是"要不要延迟"的判断：

| 位置 | 判断 |
| --- | --- |
| `Promise.h:545-563` | `IsDeferred()` / `MarkDeferred()` / `Launch()` |
| `Promise.h:674-690` | `IsDeferred()` / `IsStarted()` |
| `Promise.h:697-712` | `Start()`（含空 core 与幂等分支） |
| `Promise.h:830-846` | `ThenPromise` 的首层 + 延迟分支 |
| `Promise.h:1133-1140` | `WaitInternal` 的"漏写 Start 自动兜底" |
| `Promise.h:1310-1335` | `Append` 的首层 + 延迟登记分支（约 25 行） |
| `Promise.h:612-613` | `m_bDeferred` 原子 + `m_pLaunch` 可空指针 |

### 3.2 方案：`CDeferredPromise<TContext>`（新类型，仍放在 `Promise.h` 同文件）

- `CAsyncExecutor::BuildPromise(spCtx)` 返回 `CDeferredPromise<TContext>`；
- 该类只提供：`GetContext()` / `Then` / `ThenInline` / `ThenOn` / `Catch` / `Finally` / `ThenPromise` / `ThenBridge` / **`Start()`**；
- `Start()` 内部投递首层，**返回 `CPromise<TContext>`**（指向首层）→ 之后照常用 `Await` / `OnSettled` / `Then`；
- `CLaunchState` 变成该类的**值成员**（不再是 `shared_ptr` + 原子）：`fnLaunch` / `pFirst` / `pTarget` / `bStarted`；
- `CPromise` 侧全部删除：`IsDeferred` / `IsStarted` / `Start` / `MarkDeferred` / `RegisterFirstLayer` / `m_bDeferred` / `m_pLaunch`，以及上表 6 处判断。

**行为变化**：延迟链不能再直接 `Await()`（=`Start()` 的自动兜底取消）→ 漏写 `Start()` 变成**编译不过**（`CDeferredPromise` 没有 `Await`）。这正是"强制"的收益，但需要接受：
- `Tests/test_async_build_start.cpp` 7 例需改写（其中"漏写 Start 自动启动"用例删除）；
- `docs/common/async-usage.md` §9.1/§9.2、`async-impl.md` §5.1、`examples/main.cpp` 2 处示例改写。

**行数账（诚实估算）**：`CPromise` 侧 **-70~80 行**（含 Doxygen）；新增 `CDeferredPromise` **+80~100 行** → **净行数大致持平**。
真正的收益是：`CPromise` 的状态空间唯一化（恒"已起链"），每个层方法少一次判断，且"忘记 Start"不可能发生。
→ 因此本批次**优先级低于批次 2**，建议在批次 2 落地并稳定后再评估。

### 3.3 附加收益（若批次 2 + 3 一起做）

`NewPromise` 可以直接构造"首层 state"（`CPromise` 用私有构造 + `make_shared<CPromiseState>`），于是 `Append` 的 `m_pState == nullptr` 首层分支（`1310-1326`，约 16 行）与 `ThenPromise` 的首层分支（`830-846`，约 12 行）也能删 → 再省约 28 行，并使 `CPromise` 恒"既有 core 又有 state"。

---

## 4. 批次 4：亲和参数打包（消除非法组合，可读性收益）—— 可选

现状：`eAffinity`（`detail::HandlerAffinity` 三档）与 `pTarget`（句柄）作为两个独立参数在以下位置成对出现，且 `kAffinityChain/kAffinityInline` 时 `pTarget` 必须是空、`kAffinityExecutor` 时必须非空 —— **非法组合现在只能靠文档约束**：

- `Promise.h`：`Append(fnHandler, loc, eMode, eAffinity, pTarget)` / `RunHandler(...)` 的默认参数；
- `AsyncExecutor.h`：`ResolveExecHandle(eAffinity, pTarget, handle)`、`ShouldInline(eAffinity, pExec, bRequireIdle)`、`DispatchInlineOrPost(eAffinity, pExec, fn)`。

方案：引入 `detail::CTarget`（枚举 + 句柄，两个静态工厂 `Chain()` / `Inline()` / `On(handle)`），上述函数参数从 2 个合成 1 个。

- 收益：**非法组合不可表达**；签名短一行；`Append` 的默认参数从 2 个减到 1 个。
- 代价：净 **+5 行**左右，改动面覆盖 Promise.h / AsyncExecutor.h / Coroutine.h 的调度入口。
- 结论：**收益是"正确性 + 可读性"，不是精简行数**。建议与批次 3 一起决策，或直接不做。

---

## 5. 明确「不做」（附理由，避免反复讨论）

| 项 | 不做理由 |
| --- | --- |
| 去掉 `loc` 的默认实参（强制每次传 `ASYNC_LOC`） | 默认值不产生分支；强制只会让调用点更长，与精简无关 |
| 合并 `Await()` / `AwaitFor(ms)` | 省 1 个方法（~8 行注释），但 `AwaitFor(-1)` 可读性更差；测试/关闭路径需要超时 |
| 合并 `OnSettled` / `OnSettledOn`、`RunNotice` / `RunNoticeOn` | 用户明确要求对称保留（一条链两处对称远比"少一个方法"重要） |
| 删组合器（`WhenAny` / `WhenRace`…） | 对齐 JS 的能力集，用户明确要求 |
| 取消机制（`kCancelled`）/ 诊断带 `CSourceLoc` / 层状态瘦身（P3）/ 任务体去分配（P4） | 属于**新功能 / 新优化**，不是"强制化精简"，另立计划 |
| 批次 7（Common/Async 与 ServerCore/Exec 的边界） | 用户明确不做 |

---

## 6. 每个批次的通用验证清单（沿用既有工作方式：一步一提交一 push）

```bash
# 1) 全量 debug 构建：0 warning
./build.sh --debug Common ServerCore Tests Benchmark examples ServerExample ServerTemplate 2>&1 | grep -cE "warning:|error:"

# 2) 单元测试（计数会随批次 2/3 变化 → 同步文档）
./build/debug/tests | tail -2

# 3) release 双跑（分配护栏两种模式都要绿）
./build.sh --release Common ServerCore Tests && ./build/release/tests | tail -2

# 4) TSan（0 竞争；异步 12 个文件）
g++ -std=c++11 -fsanitize=thread -g -O1 -pthread -ICommon -ITests \
    Tests/main.cpp Tests/TestFramework.cpp Tests/test_async_*.cpp \
    Common/Async/AsyncExecutor.cpp Common/Async/Diagnostics.cpp Common/Thread/ThreadPool.cpp \
    -o /tmp/tsan_async && /tmp/tsan_async | tail -1

# 5) examples 冒烟（重点看 ⑨ 上下文、⑫ 生命周期、㉖ 分叉+协程）
./build.sh --debug examples && timeout 15 ./build/debug/example | tail -5

# 6) 格式（0 违规）
~/.local/bin/clang-format --dry-run --Werror $(git diff --name-only | grep -E '\.(h|cpp)$')

# 7) 编译数据库（改了 .cpp 集合 / 目录 / Makefile flags 才需要）
./build.sh --compiledb [受影响项目]

# 8) 基准（动了热路径时必跑；报告文件会被直接改写，随提交一起入库）
./build.sh --release Common Benchmark && ./build/release/benchmark | tail -20
```

**每批次的验收标准**：构建 0 warning + 测试全绿（debug & release）+ TSan 0 竞争 + examples 正常跑完 + clang-format 0 违规 + 文档同步（含用例计数）+ 提交信息里写明"删掉了哪些状态/分支"。

---

## 7. 影响面矩阵

| 文件 | 批次 1 | 批次 2 | 批次 3 | 批次 4 |
| --- | --- | --- | --- | --- |
| `Common/Async/Promise.h` | ✔ 已改 | ✔ 大改 | ✔ 大改 | ✔ 签名 |
| `Common/Async/AsyncExecutor.h` | ✔ 已改 | ✔ 小改 | ✔ 返回类型 | ✔ 签名 |
| `Common/Coroutine/Coroutine.h` | ✔ 已改 | — | — | ✔ 签名 |
| `Tests/*`（4 个文件） | ✔ 2 处 | ✔ 5 处（删 3 例） | ✔ build_start 7 例 | — |
| `examples/main.cpp` | ✔ ⑨ | ✔ ⑫/㉖ | ✔ 2 处 | — |
| `Benchmark/cases/*` | — | — | — | ✔（若签名变化） |
| `docs/common/async-usage.md` | ✔ §5/§14 | ✔ §5/§6.3/§14 | ✔ §9.1/§9.2 | — |
| `docs/common/async-impl.md` | ✔ §12/§11 | ✔ §6.1/§7/§10 | ✔ §5.1 | — |
| `docs/common/coroutine-*.md` | ✔ | — | — | — |
| `docs/servercore/testing-usage.md` | — | ✔ 用例表 | ✔ | — |

---

## 8. 待拍板的问题（请逐条选）

1. **批次 1 收尾**：`Context()` 返回 `const std::shared_ptr<TContext>&`（零拷贝，detail 内部使用）还是返回值（更保守）？→ 建议：**引用**。
2. **批次 2（promise 恒有效）是否做**？接受"取消默认构造 + 删 `IsValid()` + `OnSettled` 返回 `void` + 删 3 个测试用例 + 5 处用户代码改写（跨作用域持有改用智能指针）"吗？
3. **批次 3（延迟启动独立类型）是否做**？它**净行数持平**，收益是"链上不再有双形态、忘记 Start 编译不过"；需要接受 build_start 7 例改写与"延迟链必须显式 Start"。
4. **批次 4（亲和参数打包）是否做**？净 +5 行，换"非法组合不可表达"。
5. **是否按批次独立提交 + push**（沿用既有工作方式），还是希望 1+2 合并成一次较大的重构提交？
