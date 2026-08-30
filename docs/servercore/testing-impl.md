# 测试框架 — 实现文档

> 配套使用文档：[testing-usage.md](testing-usage.md)
> 源码：`Tests/TestFramework.h`、`TestFramework.cpp`

## 1. 核心结构

```cpp
struct TestCase { const char* strName; std::function<void()> fnBody; };
std::vector<TestCase>& Registry();   // 全局用例注册表（static vector）
void Register(const char* strName, const std::function<void()>& fnBody);
int RunAll();                        // 运行全部，返回失败数
```

## 2. TEST 宏（静态注册）

```cpp
#define TEST(name)                                                        \
    static void Test_##name();                                            \
    namespace {                                                           \
    struct TestReg_##name { TestReg_##name() { testfw::Register(#name, &Test_##name); } } \
        g_TestReg_##name;                                                 \
    }                                                                     \
    static void Test_##name()
```

利用**静态对象构造在 `main` 之前**：每个 `TEST` 定义在匿名命名空间的一个静态注册对象，
构造时把用例登记进全局注册表——业务代码只需写 `TEST(name) { ... }`，无需手动注册。

## 3. 断言宏（抛异常中断用例）

```cpp
#define ASSERT_TRUE(expr) \
    do { if (!(expr)) { std::printf("      FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); throw -1; } } while (0)

#define ASSERT_EQ(a, b) \
    do { if (!((a) == (b))) { ... throw -1; } } while (0)
```

- 失败打印 `文件:行号 表达式` 后 `throw -1`，由 `RunAll` 捕获标记该用例失败并**继续下一个用例**；
- **注意**：只有 `ASSERT_TRUE` / `ASSERT_EQ` 两个宏（无 `ASSERT_FALSE`，用 `ASSERT_TRUE(!x)`）；
- 断言抛异常依赖「同一线程」——**禁止在工作线程内 ASSERT**（跨线程抛异常 → `std::terminate`）。

## 4. RunAll

```cpp
int RunAll()
{
    for (case : Registry())
    {
        printf("[ RUN  ] %s\n", name);
        try { fnBody(); printf("[ PASS ] %s\n", name); ++nPass; }
        catch (...) { printf("[ FAIL ] %s\n", name); ++nFail; }
    }
    printf("total=%zu  pass=%d  fail=%d\n", ...);
    return nFail;   // 退出码 = 失败数（0 表示全通过）
}
```

`main()` 返回 `RunAll()` 的结果，因此失败用例数即进程退出码（脚本/CI 可直接判断）。

## 5. 设计约束（本仓库约定）

- **主线程断言**：断言在测试线程执行；并发测试中工作线程只写原子状态，主线程 `WaitUntil` 后断言；
- **无第三方测试框架**：零依赖、单文件、可被 `Tests/Linux/Makefile` 的 `find` 自动纳入；
- **基准/压力**：独立的 `Benchmark/` 项目（`./build/release/benchmark`），不混入单元测试。
