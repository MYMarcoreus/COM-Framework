---

name: build-system
description: 本项目的 Ubuntu 24.04、Docker、Makefile、generate-compiledb.sh、build-all.sh、compiledb、Bear 和 clangd 构建规范。修改构建系统、Makefile、Dev Container、编译数据库或解决编译问题时使用。
----------------------------------------------------------------------------------------------------------------------------------------------

# 构建系统规范

## 1. 标准环境

项目标准环境：

```text
WSL
  ↓
Ubuntu 24.04
  ↓
Docker / Dev Container
  ↓
GCC / G++
  ↓
Make
```

不要假定 Windows 本机编译环境与容器一致。

---

## 2. Dev Container

开发环境由：

```text
.devcontainer/devcontainer.json
Dockerfile
```

定义。

修改构建环境前必须检查现有配置。

不要随意替换基础镜像。

不要为了单个依赖问题破坏整个开发环境。

---

## 3. Makefile

每个项目原则上拥有：

```text
<Project>/Linux/Makefile
```

Makefile 负责：

* 编译 `.cpp`
* 生成 `.o`
* 设置头文件路径
* 链接静态库
* 生成最终目标
* 清理构建产物

使用：

```make
CXX
CPPFLAGS
CXXFLAGS
LDFLAGS
LDLIBS
```

等标准变量。

禁止硬编码本机绝对路径。

---

## 4. C++11

Makefile 必须保证：

```text
-std=c++11
```

或者使用等价配置。

禁止默认改为：

```text
-std=c++17
```

或更高版本。

---

## 5. Workspace 构建

工作区根目录存在：

```text
build-all.sh
```

它负责调用指定项目的 Make 构建。

整体流程：

```text
build-all.sh
    ↓
<Project>/Linux/Makefile
    ↓
编译
    ↓
链接
```

修改项目名称、路径或构建目标时，必须同步检查 `build-all.sh`。

---

## 6. generate-compiledb.sh

项目可以通过：

```text
generate-compiledb.sh
```

提供编译数据库（compile_commands.json）生成入口。

VS Code 可以通过：

```text
tasks.json
```

调用：

```text
generate-compiledb.sh
    ↓
Makefile
```

不要在 `tasks.json` 中重复实现 Makefile 已经完成的编译逻辑。

---

## 7. VS Code Task

推荐：

```text
tasks.json
    ↓
generate-compiledb.sh
    ↓
Makefile
```

而不是：

```text
tasks.json
    ↓
g++ ...
```

构建逻辑应集中在 Makefile。

---

## 8. clangd

项目使用：

```text
clangd
```

而不是 VS Code C++ IntelliSense 作为主要 C++ 代码分析工具。

必须维护：

```text
compile_commands.json
```

---

## 9. CompileDB / Bear

支持使用：

```text
compiledb
```

或者：

```text
bear
```

生成：

```text
compile_commands.json
```

生成方式必须与实际 Make 构建保持一致。

不要手工填写虚假的编译参数。

---

## 10. clangd 问题排查

如果 `.cpp` 正常而 `.h` 出现大量红线：

优先检查：

1. `compile_commands.json`
2. `-I` 头文件路径
3. 编译器参数
4. `-std=c++11`
5. 预处理宏
6. clangd 工作目录

不要立即修改源代码。

要区分：

```text
clangd 解析错误
```

和：

```text
真正的编译错误
```

---

## 11. 静态库

公共基础库和第三方库优先生成：

```text
libxxx.a
```

然后由服务器项目链接。

推荐：

```text
源码
 ↓
.o
 ↓
libxxx.a
 ↓
ServerCore / Server
```

避免每个服务器重复编译相同的第三方源码。

---

## 12. 构建验证

修改 Makefile 后至少检查：

```text
make clean
make
```

如果存在依赖项目：

```text
Common
 ↓
ServerCore
 ↓
Server
```

则必须验证完整依赖链。

---

## 13. 禁止事项

除非明确要求：

* 不引入 CMake。
* 不引入 Meson。
* 不使用 Windows MSBuild。
* 不修改为 C++17。
* 不在任务文件中复制 Makefile。
* 不使用绝对路径。
* 不关闭 clangd 来隐藏问题。
