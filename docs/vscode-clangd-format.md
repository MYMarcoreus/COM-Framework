# VS Code + clangd：格式化与缩进约定

> 面向本仓库（多项目 Makefile 工作区）的 C++ 开发者。
> 相关文件：[`.clang-format`](../.clang-format)、[`.clangd`](../.clangd)、[`.vscode/settings.json`](../.vscode/settings.json)

## 1. 三个配置各管什么

| 文件 | 作用 |
| --- | --- |
| `.vscode/settings.json` | `[cpp]` / `[c]` 默认格式化器 = clangd、保存即格式化；缩进 4 空格、120 列标尺、关闭 `detectIndentation` |
| `.clangd` | clangd 自身配置：`FormatStyle: file`（用项目 `.clang-format`）、不显示「头文件可省略」提示；编译数据库按「源文件所在目录向上查找」自动识别 |
| `.clang-format` | 真正的排版规则：4 空格 / Allman / 120 列 / `LambdaBodyIndentation: OuterScope` / **函数体与 lambda 体都不写单行**（`AllowShortFunctionsOnASingleLine: None`、`AllowShortLambdasOnASingleLine: None`） |

**要求 clangd ≥ 13**（`.clang-format` 用了 `LambdaBodyIndentation`，13 起支持；本机为 18）。
检查版本：`clangd --version`。

## 2. 常用操作

- 单个文件：`Shift+Alt+F`（或命令面板 → `Format Document`）；保存时已自动格式化；
- 只格式化选中片段：`Ctrl+K Ctrl+F`；
- 整仓库：VS Code 没有「格式化整个仓库」命令，需要 `clang-format` CLI（本机未安装）：

```bash
find . -name '*.cpp' -not -path './build/*' -not -path './ThirdParty/*' | xargs clang-format -i
# 跑完务必 git diff 复核（并把第三方子模块排除在外）
```

## 3. 「缩进不对」的常见原因（按发生频率）

1. **clangd 版本 < 13**：`LambdaBodyIndentation` 不生效，lambda 体缩进被排错 →
   升级 clangd（扩展可自动下载，或 `apt install clangd-18`）；
2. **选了别的格式化器**：命令面板 → `Format Document With...` 选了 C/C++ 扩展（会记住上次选择），
   本仓库默认已设为 clangd，可在「设置 → 文本编辑器 → 格式化」里恢复；
3. **`editor.detectIndentation` 打开**：VS Code 会按文件内容猜 tabSize（看到 2 空格就按 2 排），
   本仓库已关闭；
4. **编译数据库过期**：clangd 拿不到 `-I` / `-std`，代码按错误解析，格式化也会乱 →
   重刷：`./build.sh --compiledb [项目...]`。

## 4. 长参数行 / lambda 的排版

`.clang-format` 采用 `AlignAfterOpenBracket: DontAlign`（续行不对齐到开括号，
只缩进一个 `ContinuationIndentWidth` 即 4 格）+ `LambdaBodyIndentation: Signature`
（lambda 体与签名同缩进）。
因此**长实参列表里直接内联 lambda** 也是安全的：实参换行后缩进一级，
lambda 体再缩进一级，结尾 `});` 与 lambda 起始列对齐，不需要再「先起名再串链」来回避对齐问题。
具名变量仍有价值 —— 当 lambda 需要**复用**、需要在多处引用同一处理器时：

```cpp
// 推荐：lambda 需要在多处复用时才先赋给具名变量（类型用 ThenHandler / PromiseFactory / PromiseExecutor）
COrderPromise::ThenHandler fnValidate = [](no::CPromiseResult upResult, const std::shared_ptr<COrderContext>& spCtx)
{
    if (upResult.IsRejected())
    {
        return upResult;
    }
    return no::CPromiseResult::Resolve();
};

return exec.NewPromise(spCtx, &StepLoadOrder, ASYNC_LOC)  // ① 具名异步函数
    .Then(fnValidate, ASYNC_LOC)                          // ② lambda
    .ThenPromise(fnQueryStock, ASYNC_LOC)                 // ③ 内部执行其他异步函数（等它）
    .Finally(&StepAudit, ASYNC_LOC);
```

```cpp
// 同样推荐：只用一次时直接内联 —— 实参缩进一级、lambda 体再缩进一级、`});` 与 lambda 对齐
const bool bOk = pUpState->AddHandler(pCore->Handle(),
    [pCore, pNextState, fnFactory](const CPromiseResult& upResult)
    {
        if (upResult.IsRejected())
        {
            return;  // 失败即停。
        }
        Adopt(pCore, pNextState, fnFactory);
    });
```

## 5. 函数体不写单行 + 每层标号

两条约定（`.clang-format` 已固化，保存即生效）：

1. **函数体 / lambda 体不写在单行**：`AllowShortFunctionsOnASingleLine: None` +
   `AllowShortLambdasOnASingleLine: None`。类内 `bool Ok() { return m_bOk; }`、
   `[&]{ return DoIt(); }` 都会被展开成多行（只保留空体 `{}`）：

```cpp
// 不推荐（仓库内尚有存量，改到哪个文件顺便展开）
CStockModule() : m_exec(1) { m_exec.Start(); }
COrderP::PromiseFactory fnQueryStock = [&](const std::shared_ptr<COrderCtx>& sp) { return Bridge(sp); };

// 推荐
CStockModule() : m_exec(1)
{
    m_exec.Start();
}
COrderP::PromiseFactory fnQueryStock = [&exec](const std::shared_ptr<COrderCtx>& sp)
{
    return Bridge(exec, sp);
};
```

2. **每个处理器 / 每一层都标号**（① ② ③ ④ ⑤ + catch + finally），让「链上的层」与
   「实现它的函数」一眼对应：具名 handler 用 `/// ① 读订单`，lambda 用 `// ② lambda：校验`，
   链上每行行尾写同一个号：

```cpp
    return exec
        .NewPromise(sp, &StepLoad, ASYNC_LOC)  // ① 具名异步函数
        .Then(fnValidate, ASYNC_LOC)           // ② lambda：校验
        .ThenPromise(fnQueryStock, ASYNC_LOC)  // ③ 调库存模块（等它）
        .ThenPromise(fnReserve, ASYNC_LOC)     // ④ 内层链（等它）
        .Then(fnBilling, ASYNC_LOC)            // ⑤ 旁支（不等它）
        .Catch(fnCompensate, ASYNC_LOC)        // catch：仅被拒绝时执行
        .Finally(fnAudit, ASYNC_LOC);          // finally：成败都跑
```

完整示例见 `examples/cases/ThenMixCase.cpp`（一条链里混用具名 handler / lambda / lambda 内执行其他异步函数），
精简版见 [common/async-mixed-then-example.md](common/async-mixed-then-example.md)。

## 6. 为什么定稿 `DontAlign`（而不是 `Align` / `BlockIndent`）

`AlignAfterOpenBracket` 是**全局单值**，clang-format 无法把「函数形参表」与「实参表」分开设置。
本仓库大量在实参列表里内联 lambda，于是三种取值只能得到：

| 配置 | 函数形参 | lambda 实参 |
| --- | --- | --- |
| `Align` | 按开括号对齐 | 跟其它实参对齐（实测 ~115 处在 17 列后，~52 处被推到 30–70 列） |
| **`DontAlign`（采用）** | 缩进一级 | 缩进一级（实参缩进一级、lambda 体再一级、`});` 与 lambda 对齐） |
| `AlwaysBreak` / `BlockIndent` | 换行后缩进，`BlockIndent` 还会把 `)` 单独断行 | 缩进一级 |

即「形参对齐」与「lambda 实参只缩进一级」在同一份配置里**不能同时自动满足**。
本仓库选**统一缩进一级**：规律简单、不看前文、不会随表达式长短改变形状。
函数形参随之也缩进一级 —— 这是刻意的取舍，不是遗漏：

```cpp
    void RunHandler(const std::shared_ptr<CPromiseState>& pState, const ThenHandler<TContext>& fnHandler,
        const CPromiseResult& upResult, int nMode, int nAffinity = kAffinityChain,
        const std::shared_ptr<CExecutorHandle>& pTarget = nullptr) const
```

### 6.1 试过、但不可靠/不成立的变通（不要再试）

- **`PenaltyIndentedWhitespace` 折中**（`Align` + 该罚分，让深对齐的实参自动换成换行缩进）：
  看起来能「形参对齐 + lambda 缩进」，但阈值不稳定 —— 实测 `ResolveExecHandle`
  / `MakeHandlerRunner` 这类长形参的自由函数声明**也跟着**被改成缩进，同一文件里两种形状混杂。
- **靠 penalty 强制调用在开括号后换行**（`PenaltyBreakBeforeFirstCallParameter`、
  `PenaltyBreakOpenParenthesis`）：实测无效，clang-format 仍然对齐。
- **`AllowAllParametersOfDeclarationOnNextLine: false`**：想让声明拒绝「整体换行」而保持对齐，实测无效。
- **`LambdaBodyIndentation: OuterScope`**：只把 lambda 体拉回外层缩进，`[` 仍在深列对齐，观感更乱。
- **唯一能强制「开括号后换行」的写法是紧跟在 `(` 后面的注释**（`AddHandler(  //`）——
  行尾注释、块注释、行前独立注释都无法阻止 clang-format 把实参拉回去对齐。
  因此「按现场强制换行」需要逐个调用点加标记（约 50 处），代价大于收益，未采用。

### 6.2 改动该选项时的纪律

切换该选项会**影响全仓库的续行排版**，需要在同一个提交里做一次全量重排，否则新旧风格混杂：

```bash
FILES=$(git ls-files '*.h' '*.cpp' | grep -v '^ThirdParty/') && clang-format -i --style=file $FILES
clang-format --style=file --dry-run --Werror $FILES   # 校验：0 违规
```

历史：`Align` → `DontAlign` 见提交 `style: 续行不对齐开括号（AlignAfterOpenBracket: DontAlign）`
（56 文件重排 + 0 违规 + 145 例全绿）。
