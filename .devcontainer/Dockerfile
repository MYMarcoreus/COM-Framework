# ==================================================================
# C++ 开发容器镜像 —— 依据 .github/skills/build-system/SKILL.md 规范
#
# 标准开发/构建环境：
#   WSL → Ubuntu 24.04 → Docker / Dev Container → GCC/G++ → Make
#
# 镜像内容：
#   - 编译工具链：gcc / g++ / build-essential
#   - 构建工具：make
#   - 编译数据库生成：bear（系统包）、compiledb（pipx 全局安装）
#   - C++ 代码分析：clangd / clang
#   - 辅助开发工具：git / gdb
# ==================================================================
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

# 安装编译工具链、make、bear、clangd 与辅助工具
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        gcc \
        g++ \
        make \
        bear \
        clangd \
        clang \
        git \
        gdb \
        pipx \
    && rm -rf /var/lib/apt/lists/*

# compiledb：从 make 构建输出生成 compile_commands.json（供 clangd 使用）
# 通过 PIPX_BIN_DIR 全局安装，使所有用户（含下面的非 root 开发用户）可用
ENV PIPX_HOME=/opt/pipx \
    PIPX_BIN_DIR=/usr/local/bin
RUN pipx install compiledb

# 直接复用基础镜像自带的非 root 用户 ubuntu（uid/gid=1000）。
# 与宿主机 WSL 用户（yangyue，uid=1000）保持一致，
# 使容器内构建产物（build/、compile_commands.json 等）在宿主机上可直接读写，
# 避免出现 root 属主文件无法清理的问题。
# 若宿主机用户 uid 不是 1000，请同步修改镜像或用户映射。
WORKDIR /workspace

# 以非 root 用户运行（最小权限原则）
USER ubuntu

CMD ["bash"]
