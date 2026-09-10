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

## 4. 长参数行 / lambda 的排版建议（踩坑点）

`.clang-format` 采用 `AlignAfterOpenBracket: Align`（续行对齐到开括号）+
`LambdaBodyIndentation: OuterScope`（lambda 体按外层作用域缩进）。
当**很长的实参列表里直接内联 lambda** 时，clang-format 会把续行推到 40+ 列，
而 lambda 体的花括号又落回外层缩进 —— 观感就是「缩进乱了」。约定写法是「先起名、再串链」：

```cpp
// 推荐：lambda 先赋给具名变量（类型用 ThenHandler / PromiseFactory / PromiseExecutor），再用变量串链
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
// 不推荐：签名很长的 lambda 作为实参直接内联 —— 续行会被对齐到开括号、lambda 体又掉回外层缩进
return CPromise<Ctx>::New(exec, spCtx, [&](const CPromise<Ctx>::ResolveFn& fnResolve,
                                           const CPromise<Ctx>::RejectFn& fnReject) { /* … */ },
                          ASYNC_LOC);
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

## 5. 想把长参数列表改成「整块缩进」怎么办

把 `.clang-format` 的 `AlignAfterOpenBracket: Align` 改成 `BlockIndent`（clang-format ≥14）即可：
续行会改为换行后按 4 空格整块缩进、不再对齐到开括号。注意这会**影响全仓库的函数签名排版**，
需要一次全量 `clang-format -i` 重排后再提交，否则新旧风格混杂。
