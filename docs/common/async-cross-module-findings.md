# 跨模块异步的两个真问题（实测记录）

`Tests/test_async_modules.cpp` / `Tests/test_async_modules_stress.cpp` / `Tests/test_async_affinity.cpp`
用「每个模块自持 1 线程执行器、跨模块只交换 promise + 上下文」的方式压了一轮（共 20 个用例），
暴露了两个此前没写下来的真问题。两个都是**实测复现**、不是推理，记录在这里备查。
问题 ① 已由改进 A（线程亲和）修复；② 已由框架层的「通知送达保证」修复（调用方不再需要写兜底代码）。

复现环境：`./build.sh --debug Tests && ./build/debug/tests`（模块相关 11 例，约 0.5 s）。

| 问题 | 现象 | 影响面 | 现状 |
|---|---|---|---|
| [① 跨模块返回后的续跑线程二选一](#一跨模块返回后的续跑线程二选一不确定) | 同一段代码有时跑在**被调模块线程**、有时跑在**本模块线程** | 任何"假设续跑线程"的代码（无锁改数据、线程亲和） | ✅ **已修复**（改进 A：线程亲和，2026-09-11） |
| [② 手写桥接不检查 `OnSettled` 返回值 → 死等](#二手写桥接不检查-onsettled-返回值--本层永久-pending--上层-await-死等) | 被调模块已停止时，上层 `Await()` **永久阻塞**（测试进程挂住） | 手写 `OnSettled` 桥接的每一处 | ✅ **已修复**（框架保证送达，调用方无需检查） |

---

## 一、跨模块返回后的续跑线程二选一（不确定）

### 1.1 现象（修复前）

一条跨模块链：`A1 → 等库存模块（B1/B2）→ A2 → 显式 Post 回本模块 → A3`。
其中 **A2（跨模块返回后的那一层）跑在哪个线程上不确定**：

- 有时在**库存模块的 worker** 上（= 结算它的那条线程）；
- 有时在**订单模块的 worker** 上（= 本链自己的执行器）。

实测（200 条链并发调同一个单线程库存模块，`ModuleStress_ManyConcurrentChains`）：

```text
200 条链：续跑在库存模块线程 57 条 / 在订单模块线程 143 条
```

同一个二进制多跑几次，这个比例还会浮动（57:143、61:139…），但**两种都会出现**。

> **修复后（2026-09-11，改进 A：线程亲和）**：同一用例变成
> `200 条链：续跑在本模块线程 200 条 / 在结算线程 0 条`，且断言已收紧为「恒在本模块执行器线程」
> （`Tests/test_async_modules_stress.cpp` 与 `Tests/test_async_affinity.cpp`）。修复方式见 [1.4](#14-修复线程亲和改进-a已完成)。

### 1.2 根因：处理器有两个可能的执行线程

`Common/Async/Promise.h` 里，层与层之间的推进只有两条路径：

```cpp
// CPromiseState::AddHandler —— 登记处理器
if (!m_bSettled.load(std::memory_order_relaxed))
{
    m_vecHandlers.push_back(std::move(fnHandler));  // ① pending：登记，settle 时触发
    return true;
}
bFireNow = true;
result = m_result;
// ② 已 settled：投递到「本链执行器」异步执行
if (PostToHandle(pHandle, std::move(fnRun)))
{
    return true;
}
return false;  // 已 settled 但执行器不可用 → 处理器不会执行
```

```cpp
// CPromiseState::Settle —— 结算本层
m_cv.notify_all();
for (size_t i = 0; i < vecHandlers.size(); ++i)
{
    vecHandlers[i](result);  // 在「结算线程」上按注册顺序内联跑完 → 逐层级联
}
```

于是对**同一个层**：

- 处理器在 settle **之前**登记（路径 ①）→ 在**结算线程**上内联执行 → 表现为"跨模块续跑落在对方模块线程"；
- 处理器在 settle **之后**才挂（路径 ②，`Then` / `Catch` / `Finally` / `ThenPromise` 都可能）→ 投递回**本链执行器**
  → 表现为"续跑回到自己模块线程"。

### 1.3 为什么调用方会"追不上"（修复前的成因）

链是**边跑边搭**的：`exec.NewPromise(...)` 一返回就把首层投递出去了（"首层在调用返回后异步执行"），
后续 `Then` / `ThenPromise` 是**调用线程**在追加。被调模块很快时（步骤里没有耗时），
执行速度可能超过调用方的搭建速度，尤其是并发链多、被调模块 worker 已经在自旋等任务的时候。

修复前，三个用例把两种情形**分别钉死**（确定性）；**修复后**它们的期望已改为"恒在本模块线程"：

| 用例 | 条件 | 修复前 | 修复后 |
|---|---|---|---|
| `ModuleStress_DeepRoundTrips` | 被调模块每步 `sleep 2 ms`（挂下一层远早于 settle） | 100 轮全在结算线程 | 100 轮**全在本模块线程** |
| `ModuleStress_LateAppendRunsOnOwnExecutor` | 链**已经跑完**再挂下一层 | 必然投递回本链执行器 | 不变（仍投递回本链执行器） |
| `ModuleStress_ManyConcurrentChains` | 被调模块 0 延迟、200 条并发 | 两选一（实测 57:143） | **100% 本模块线程** |

### 1.4 修复：线程亲和（改进 A，已完成）

思路：**把"层在哪跑"从隐式改成显式绑定** —— 层只在它所属链的执行器线程上执行；
当前线程已经是本链执行器线程时就地内联（保留顺序与性能），否则一律投递回本链执行器。

```cpp
// Common/Thread/ThreadPool：worker 线程打 thread_local 标记（进入设、退出清）
thread_local const CThreadPool* tl_pCurrentPool = nullptr;
static bool CThreadPool::IsInPoolThread(const CThreadPool* pPool);

// Common/Async/AsyncExecutor.h（detail）
inline bool IsInExecutorThread(const std::shared_ptr<CExecutorHandle>& pHandle)
{
    return pHandle != nullptr && CThreadPool::IsInPoolThread(pHandle->m_pPool.get());
}

// Common/Async/Promise.h：层处理器（CPromiseCore::RunHandler）
if (IsInExecutorThread(pCore->Handle()) && InlineDepth() < kMaxInlineDepth)
{
    ++InlineDepth();
    fnRun();
    --InlineDepth();  // 同执行器：就地内联
}
else if (!PostToHandle(pCore->Handle(), std::move(fnRun)))
{
    pState->Settle(CPromiseResult::Reject(kStopped));  // 跨执行器：投递回本链执行器；不可用则拒绝
}

// Common/Coroutine/Coroutine.h：协程续跑（ResumeInline）用同一判定
if (detail::ShouldInline(detail::kAffinityChain, m_pExec->Handle(), true))
{
    Resume();
}
else
{
    PostResume();
}
```

结论与代价：

- 修复后的保证：**每一层 + 协程的每一次续跑，都跑在它所属链的执行器线程上**；
  跨模块返回也不再依赖"谁 settle"（200 条并发链实测 100% 回本模块线程）；
- 代价：每次跨执行器的续接多一次入队 + 唤醒（微秒级）；同执行器内仍完全内联，不受影响；
  内联深度也只在同一执行器线程内累加，跳模块不会涨栈；
- 边界（**亲和只作用于"层"**）：`OnSettled` 是通知 → 仍在**结算线程**（跨模块时=被调模块线程）上触发；
  `exec.NewPromise(spCtx, executor)` 的 executor 是"发起"语义 → 仍在调用线程上同步执行；`Await()` 仍占住调用线程；
- 验收：`Tests/test_async_affinity.cpp`（5 例：0 延迟 / 慢被调 / 50 轮往返 / 协程跨模块 await /
  4 条并发链），以及被改紧的原极限用例（`ModuleStress_*`）。

### 1.5 什么仍然是确定的

- **顺序**：由依赖边保证（内层 settle → 桥接回调 → 本层 settle → 外层下一层，之间有 happens-before），
  与线程无关。200 条并发链的轨迹仍然逐条精确等于 `A1;B1;B2;A2;A3;`。
- **模块内不重叠**：单线程执行器就是排队（实测并发峰值恒为 1，含 64 分支并发汇聚、200 条并发链）。
- **模块线程固定**：同一模块的所有"自有步骤"始终落在同一个 worker 上。

### 1.6 写法建议（即使在已修复的前提下）

1. **层处理器可以直接操作本模块状态**（亲和已保证在本模块线程）；但 **`OnSettled` 回调不要碰本模块状态**
   —— 它跑在被调模块线程上，只做「语义转换 + 改上下文（原子/锁） + settle」；
2. 上下文只被"自己那条链"写，或跨线程字段用原子/锁（`OnSettled` 仍然跨线程）；
3. **回调里只做轻活**，不要在里面做重活/阻塞等待；
4. 不要用 `std::this_thread::get_id()` 做业务判断（做自校验、断言可以）；
5. 要额外强调"必须回本模块"时，显式 `exec.Post(...)` 仍然可用（现已是冗余保险，
   见 `PostBackToOwnThread`）；
6. 写测试时**不要再写"续跑一定在对方线程"的断言**（改前很容易写成这样）；改后应为
   "跨模块返回的层必在本模块线程"、"`OnSettled` 通知在被调模块线程"；
7. 跨模块那一层**优先用 `p.ThenBridge(fnCreate, fnApply, ASYNC_LOC)`**（2026-09-12 新增）：
   起子链 + 搬数据一步到位，不用手写 `New` + `OnSettled` 样板（也不再需要 `if (!bOk)` 保险）。
   子链被拒绝时的码原样透传；业务规则上的拒绝写到**桥接之后的层**里。
   对照用例：`Tests/test_async_modules.cpp` 的 `Module_BridgeHelper*`（与手写版逐项等价）。

### 1.7 后续可做的改进

- ~~**B（逐层指定执行器）**：`ThenOn(exec, handler)` / `ThenInline(handler)`~~ —— **已完成（2026-09-11）**：
  亲和三档（`kAffinityChain` 默认 / `kAffinityInline` 就地 / `kAffinityExecutor` 指定）已实现，
  对外 API 为 `ThenInline` / `ThenOn`（只影响那一层，之后的层回本链执行器）；
  验收：`Tests/test_async_affinity_override.cpp`（4 例）；文档：async-usage §9、async-impl §8.1；
- **C（build-then-start）**：全链挂完再投递首层 —— **已完成（2026-09-11）**：
  新增 `CAsyncExecutor::BuildPromise(spCtx)` + `CPromise::Start()`（幂等；`Await()` 对未启动的延迟链
  自动 `Start()` 兜底；`New(...)` 的 executor 也延后到轮到该层才执行；首层启动尊重 `ThenOn` 的目标执行器）。
  验收：`Tests/test_async_build_start.cpp`（7 例）；文档：async-usage §9.2、async-impl §5.1。

---

## 二、手写桥接不检查 `OnSettled` 返回值 → 本层永久 pending → 上层 `Await()` 死等

### 2.1 现象（修复前）

场景：订单模块调库存模块，**半路把库存模块的执行器 `Stop()` 掉**（等价于对方模块被卸载/停止），
之后再发起跨模块调用（`ModuleStress_StopMidFlight`）。

- 修复前：测试进程**挂住不返回**（45 s 超时才能杀掉），既不崩溃也不报错——最难查的一类问题；
- 修复后：链以 `kStopped(=2)` 拒绝、库存模块 0 步执行、`Catch` 正常收尾，用例 0.5 s 内跑完。

### 2.2 根因：子 promise 已 settled，但它的执行器已不可用（修复前）

```cpp
// 调用方（桥接层）：把被调模块的 promise 接进本流程
common::async::CPromise<CStockCtx> promiseStock = spStockModule->QueryStockAsync(spStock);  // ①
promiseStock.OnSettled([...](common::async::CPromiseResult result) { /* 回调 */ });  // ② 返回值被丢弃 ← 病灶
```

链路一步步是：

1. 被调模块的执行器已 `Stop()` → 它内部 `NewPromise` 的投递失败 → `CPromiseCore::PostHandler` 里
   `pState->Settle(CPromiseResult::Reject(kStopped))` —— **这一步是对的**，子 promise 立即落定为 `kStopped`；
2. 调用方紧接着 `OnSettled(...)`，此时子 promise **已经 settled** → `CPromiseState::AddHandler` 走路径 ②
   → `PostToHandle(pHandle, ...)` 用的是**被调模块的执行器**（已停止）→ 返回 `false`，
   **回调永远不会执行**；
3. 桥接层（`exec.NewPromise(spCtx, executor)` 出来的那一层）**没有任何人去 settle 它** → 永久 pending；
4. 上层 `Await()` 阻塞在 `CPromiseState::Await` 的条件变量上，`m_cv.wait(...)` 永不唤醒 → **死等**。

修复前 `OnSettled` 的文档就是这么写的（"返回 `false`：本层已 settled 但执行器不可用，通知不执行"），
但桥接代码里太容易把它丢掉——丢了就是挂死，而且**只有对方模块被停止/投递被拒时才触发**，
平时测试全绿，上线才炸。

### 2.3 修复：框架保证「通知一定送达」（已完成）

不再把这件事交给调用方。新增一个「送达保证」的注册接口，专供 `OnSettled` 用：

```cpp
// Common/Async/Promise.h —— detail::CPromiseState
// ① 层处理器（then / catch / finally / thenPromise）：保持原语义
//    执行器不可用 → 返回 false，由框架以 kStopped 收口本层（“停了的执行器不再跑新层”）。
bool AddHandler(const std::shared_ptr<CExecutorHandle>& pHandle, Handler fnHandler);

// ② 通知（OnSettled）：送达保证（同一个 AddHandler，多传一个策略位）
bool AddHandler(const std::shared_ptr<CExecutorHandle>& pHandle, Handler fnHandler, bool bGuaranteedDelivery);
// AddHandler(..., /* bGuaranteedDelivery = */ true) → 已 settled 且执行器不可用时就地送达
```

于是：

- `OnSettled` **只在 promise 无效时**返回 `false`；桥接代码**不需要再检查返回值**；
- 送达位置：执行器可用 → 执行器线程（不阻塞调用方）；不可用 → **调用线程**（就地，微秒级）；
- 不会因此递归加深：通知里通常只 settle 本层，而本层后续的层处理器走 `RunHandler`，
  执行器不可用时以 `kStopped` 收口 → 链立即结束（有 `InlineDepth` 计数兼底）；
- **层的语义不变**：`Then` / `Catch` / `Finally` 在“已 settled + 执行器不可用”时依旧以 `kStopped` 结算，
  不会“就地执行一层”（验收：`SettledNotice_LayerStillRejectedWhenExecutorUnavailable`）。

验收用例（`Tests/test_async_settled_delivery.cpp`，5 例）：

| 用例 | 验的是什么 |
|---|---|
| `SettledNotice_DeliveredEvenIfExecutorStopped` | 执行器已停 → 已 settled 上注册通知：返回 true、回调就地执行、层仍不跑 |
| `SettledNotice_ManyRegistrationsAllDelivered` | 一次注册 100 个通知：全部送达、按注册顺序、均在调用线程 |
| `SettledNotice_InvalidPromiseReturnsFalse` | 唯一仍为 `false` 的情形：无效 promise |
| `SettledNotice_BridgeWithoutReturnCheckNoDeadlock` | **关键回归**：桥接漏检返回值的原形状 → 链以 `kStopped` 拒绝，不再死等 |
| `SettledNotice_LayerStillRejectedWhenExecutorUnavailable` | 对照：层不被就地执行，仍以 `kStopped` 收口 |

### 2.4 写作建议（现已可选）

检查返回值已是冗余保险，但保留也无害（不再有“漏检就挂死”的风险）：

```cpp
const bool bOk = promiseStock.OnSettled([...](common::async::CPromiseResult result) { /* 桥接回调 */ });
if (!bOk)
{
    fnReject(common::async::kStopped);  // 现只会在 promise 无效时触发；留作防御
}
```

多分支汇聚（手写 `when_all`）时仍要保证“剩余计数”被减到 0 —— 那是业务自己的计数逻辑，
与送达保证无关（`JoinBranches` 里就是这么处理的）。

---

## 三、这些结论对应的用例与验证方式

### 3.1 共享测试脚手架（`Tests/AsyncTestKit.h`）

五个跨模块测试文件（`test_async_*modules*.cpp` / `_affinity.cpp` / `_combine.cpp` / `_robustness.cpp`）
原本各写一份「观测工具 + 被调模块」，现已抽到 `Tests/AsyncTestKit.h`（`namespace asynctest`）：

| 组件 | 作用 |
|---|---|
| `CTraceSink` | 步骤轨迹 + 每步所在线程（带锁；可空，不接则不记录轨迹） |
| `CStepProbe` | 步数与并发峰值（全 `std::atomic`，可空，不接则不计） |
| `EnterOrderStep` / `LeaveOrderStep` / `EnterStockStep` / `LeaveStockStep` / `SleepMs` | 探针包装与模拟耗时（传空探针即空操作） |
| `CCalleeCtx` / `CCalleeModule` | 两步被调模块：自持 1 线程执行器、`QueryStockAsync()` / `Stop()`、可配 `nDelayMs` 与 `bReject` |

测试文件只留「本用例自己的订单模块与断言」：`test_async_affinity_override.cpp` 与
`test_async_build_start.cpp` 保留各自的一步被调模块（轨迹约定与 `B1;B2;` 不同），不强行统一。

### 3.2 用例清单

| 用例 | 覆盖 |
|---|---|
| `Tests/test_async_affinity.cpp`（5 例） | 改进 A 的验收：0 延迟 / 慢被调 / 50 轮往返 / 协程跨模块 await / 4 条并发链，均在本模块线程 |
| `Module_OrderAndThreadOwnership` | 顺序 `A1;B1;B2;A2;A3;`、模块线程互不共用、跨模块返回层回本模块 |
| `Module_SingleThreadSerializesOwnSteps` | 4 条并发查询同一单线程模块：并发峰值 = 1、轨迹 `B1;B2;` × 4 |
| `Module_ConcurrentChainsKeepOwnOrder` | 两条并发链各自顺序完整、模块线程固定 |
| `ModuleStress_ManyConcurrentChains` | 200 条并发链：轨迹逐条精确、步数精确、**续跑 100% 在本模块线程**（问题 ①） |
| `ModuleStress_DeepRoundTrips` | 50/100 轮往返：每轮都回本模块线程（问题 ①） |
| `ModuleStress_LateAppendRunsOnOwnExecutor` | 落定后再挂层 → 必然回本链执行器（问题 ① 的另一面） |
| `ModuleStress_DeepSingleChain` | 5000 层单链：压 `kMaxInlineDepth` 防爆栈 |
| `ModuleStress_ConcurrentAwaitSameChain` | 8 线程并发 `Await` 同一条链：`notify_all`、链只跑一遍 |
| `ModuleStress_FanOutJoin` | 一层分叉 64 分支 + 手写汇聚：全完成、库存串行（纯 async 可用 `exec.WhenAll` 直接写） |
| `ModuleStress_MixedRejections` | 100 条链一半被拒：成功/失败互不串 |
| `ModuleStress_StopMidFlight` | 半路 `Stop()` 被调模块 → `kStopped` 退化（问题 ②） |
| `Tests/test_async_settled_delivery.cpp`（5 例） | 问题 ② 的修复验收：通知送达保证 + 层的语义不变（含“漏检返回值不死等”回归） |

```bash
# 跑全部测试（含以上用例）
./build.sh --debug Tests && ./build/debug/tests        # total=158 pass=158 fail=0

# 数据竞争检查（异步测试文件 + 异步框架 + 线程池）
g++ -std=c++11 -fsanitize=thread -g -O1 -pthread -ICommon -ITests \
    Tests/main.cpp Tests/TestFramework.cpp \
    Tests/test_async_modules.cpp Tests/test_async_modules_stress.cpp \
    Tests/test_async_affinity.cpp Tests/test_async_affinity_override.cpp \
    Tests/test_async_build_start.cpp Tests/test_async_settled_delivery.cpp \
    Common/Async/AsyncExecutor.cpp Common/Thread/ThreadPool.cpp -o /tmp/tsan_async
/tmp/tsan_async                                       # 0 条 data race
```

> **测试代码自身的问题（已修，2026-09-12）**
> - `Tests/test_async_chain.cpp` 的 `Promise_Fork`：2 线程执行器并发跑两条分支，却让两条分支写同一个
>   非原子 `CTestContext` 字段 —— 违反「共用上下文只写不同字段」的契约，TSan 报 data race。
>   已改为两条分支各写自己的字段（`nForkA` / `nForkB`，`std::atomic`）。
> - `Tests/test_async_combine.cpp` 的 `MakeBranchStep`：多条分支并发写同一个 `idBranch`（`thread::id`）
>   —— 已删该字段，分支线程从 `CTraceSink::ThreadOf()` 读（轨迹里本来就记了每步线程）。
> - `Tests/test_async_affinity_override.cpp` 的 `ThenInline` 用例原依赖「登记时上游尚未 settle」的时间窗
>   （早期靠固定 `nDelayMs`，在 TSan ~10 倍减速下偶发失败；且那些 `nDelayMs` 字段其实从未被读）。
>   排查后发现**该用例的断言本身不可确定**：跨模块桥接的结算线程是二选一的（子 promise 若在 `Adopt`
>   注册通知前就落定，通知会被投递回本链执行器）。已改为由用例**自己指定结算线程**（子 promise 建在
>   旁路执行器上，挂完层后再投递结算），断言从「不是本链线程」升级为「等于结算线程」。
>   跨模块的真实形状由 `RunDefaultAsync` / `RunOnSideAsync` 与 `test_async_affinity.cpp` 覆盖。
>   验证：TSan 连跑 5 轮，0 竞争、异步 112 例全绿。
