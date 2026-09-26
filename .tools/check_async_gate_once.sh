#!/usr/bin/env bash
set -euo pipefail

# ====================================================================
# 编译期自检：`ASYNC_GATE` 在同一个函数里只允许出现一次
#
# 为什么要有这个检查：
#   宏的重复使用 = 「同一层体既读又写」——运行时会读 / 写换档**来回重入**（框架会用「重复挂起」收口，
#   但那是有界失败，不是正确用法）。宏里带一个固定标签 `ASYNC_GATE_ONCE_PER_FUNCTION`，同一函数内
#   第二次出现必然 `duplicate label`；本脚本把这条性质钉住：日后重构宏时若不小心丢掉它，这里立刻红。
#
# 用法：bash .tools/check_async_gate_once.sh        （退出码 0 = 通过）
#
# 说明：
#   - 只做语法检查（-fsyntax-only），不产出目标文件，秒级完成；
#   - 检查对象是**宏 + 被检源码**（`-ICommon`），与业务代码用同一份头文件。
# ====================================================================

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

FLAGS=(-std=c++11 -Wall -Wextra -fsyntax-only -I"${ROOT}/Common")
FAILS=0

# ---------------------------------------------------------------
# 用例 1：单个函数用一次 → 必须编译通过（且零告警）
# ---------------------------------------------------------------
cat > "${WORK}/once.cpp" <<'CPP'
#include "Async/AsyncExecutor.h"
#include "Async/GateGuard.h"

common::async::CPromiseResult StepOnce()
{
    ASYNC_GATE_WRITE();
    return common::async::CPromiseResult::Resolve();
}
CPP

if OUT="$(g++ "${FLAGS[@]}" "${WORK}/once.cpp" 2>&1)" && [[ -z "${OUT}" ]]; then
    echo "✓ 单次使用：编译通过、零告警"
else
    echo "✗ 单次使用：期望「编译通过 + 零告警」，实际输出："
    echo "${OUT}"
    FAILS=$((FAILS + 1))
fi

# ---------------------------------------------------------------
# 用例 2：两个不同函数各用一次 → 必须编译通过（标签是函数作用域，不误伤）
# ---------------------------------------------------------------
cat > "${WORK}/twofunc.cpp" <<'CPP'
#include "Async/AsyncExecutor.h"
#include "Async/GateGuard.h"

common::async::CPromiseResult StepRead()
{
    ASYNC_GATE_READ();
    return common::async::CPromiseResult::Resolve();
}

common::async::CPromiseResult StepWrite()
{
    ASYNC_GATE_WRITE();
    return common::async::CPromiseResult::Resolve();
}
CPP

if OUT="$(g++ "${FLAGS[@]}" "${WORK}/twofunc.cpp" 2>&1)" && [[ -z "${OUT}" ]]; then
    echo "✓ 两个函数各一次：编译通过（不误伤）"
else
    echo "✗ 两个函数各一次：期望编译通过，实际输出："
    echo "${OUT}"
    FAILS=$((FAILS + 1))
fi

# ---------------------------------------------------------------
# 用例 3 / 4：同一函数里用两次 → 必须编译失败，且失败原因是 duplicate label
# ---------------------------------------------------------------
check_dup_rejected()
{
    local SRC="$1"
    local NAME="$2"
    local OUT

    if OUT="$(g++ "${FLAGS[@]}" "${SRC}" 2>&1)" && [[ -z "${OUT}" ]]; then
        echo "✗ ${NAME}：期望编译失败（duplicate label），实际编译通过"
        FAILS=$((FAILS + 1))
        return
    fi
    if ! grep -q "duplicate label" <<<"${OUT}"; then
        echo "✗ ${NAME}：确实编译失败了，但不是 duplicate label："
        echo "${OUT}"
        FAILS=$((FAILS + 1))
        return
    fi
    echo "✓ ${NAME}：编译期拦下（duplicate label）"
}

cat > "${WORK}/twice_samescope.cpp" <<'CPP'
#include "Async/AsyncExecutor.h"
#include "Async/GateGuard.h"

common::async::CPromiseResult StepTwiceSameScope()
{
    ASYNC_GATE_READ();
    ASYNC_GATE_WRITE();
    return common::async::CPromiseResult::Resolve();
}
CPP
check_dup_rejected "${WORK}/twice_samescope.cpp" "同一作用域两次（读 + 写）"

cat > "${WORK}/twice_nested.cpp" <<'CPP'
#include "Async/AsyncExecutor.h"
#include "Async/GateGuard.h"

common::async::CPromiseResult StepTwiceNested()
{
    {
        ASYNC_GATE_READ();  // 藏在块里也拦得住：标签是**函数**作用域，不是块作用域
    }
    ASYNC_GATE_WRITE();
    return common::async::CPromiseResult::Resolve();
}
CPP
check_dup_rejected "${WORK}/twice_nested.cpp" "嵌套作用域两次"

# ---------------------------------------------------------------
# 汇总
# ---------------------------------------------------------------
if [[ "${FAILS}" -ne 0 ]]; then
    echo "ASYNC_GATE 唯一性检查：失败 ${FAILS} 项"
    exit 1
fi
echo "ASYNC_GATE 唯一性检查：通过（4/4）"
