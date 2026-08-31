#!/usr/bin/env bash
# ====================================================================
# DataHub 一键脚本：编译 + 配置外部访问 + 启动
#
# 用法：
#   ./run.sh                —— 编译（debug）+ 配置端口转发 + 启动
#   ./run.sh --release      —— 编译（release）+ 配置端口转发 + 启动
#   ./run.sh --no-forward   —— 跳过端口转发配置（纯本机运行）
#   ./run.sh --stop         —— 停止服务器并清理端口转发
#
# 说明：
#   - 编译：调用根目录 ./build.sh 构建 DataHub（默认 debug）。
#   - 端口转发：若运行在 WSL2 中，自动调用 Windows netsh 添加端口转发
#     （Windows <端口> → WSL <IP>:<端口>），使局域网设备（如手机）可访问。
#   - 若不在 WSL 环境（纯 Linux），直接启动，监听 0.0.0.0 即可被局域网访问。
#   - 默认端口从 DataHub/datahub.ini 的 [server] port 读取；可用 -p 覆盖。
# ====================================================================
set -euo pipefail

# —— 路径与参数 ——
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # 工作区根
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"       # DataHub 目录
MODE="debug"
MODE_FLAG="--debug"
DO_FORWARD=1
ACTION="start"
PORT=""

# —— 解析参数 ——
while [[ $# -gt 0 ]]; do
    case "$1" in
        --release|-r) MODE="release"; MODE_FLAG="--release"; shift ;;
        --debug|-d)   MODE="debug";   MODE_FLAG="--debug";   shift ;;
        --no-forward) DO_FORWARD=0; shift ;;
        --stop|-s)    ACTION="stop"; shift ;;
        -p)           PORT="${2:?用法: -p <端口>}"; shift 2 ;;
        -h|--help)    sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "未知参数: $1（-h 查看帮助）"; exit 1 ;;
    esac
done

# —— 读取默认端口（datahub.ini 的 [server] port，默认 8888）——
if [[ -z "$PORT" ]]; then
    PORT="$(grep -E '^\s*port\s*=' "$DIR/datahub.ini" 2>/dev/null | head -1 | awk -F= '{gsub(/[ \t]/,"",$2); print $2}')"
fi
PORT="${PORT:-8888}"

# —— 判断是否在 WSL2 中 ——
IN_WSL=0
if [[ -f /proc/version ]] && grep -qi 'microsoft' /proc/version; then
    IN_WSL=1
fi

# —— 停止：杀掉服务器 + 清理端口转发 ——
if [[ "$ACTION" == "stop" ]]; then
    echo "【stop】停止 datahub ..."
    pkill -f "build/$MODE/datahub" 2>/dev/null || true
    pkill -f "build/release/datahub" 2>/dev/null || true
    pkill -f "build/debug/datahub" 2>/dev/null || true
    if [[ $IN_WSL -eq 1 ]] && command -v netsh.exe >/dev/null 2>&1; then
        echo "【stop】清理 Windows 端口转发 :$PORT ..."
        netsh.exe interface portproxy delete v4tov4 listenport="$PORT" listenaddress=0.0.0.0 >/dev/null 2>&1 || true
    fi
    echo "【stop】完成。"
    exit 0
fi

# —— ① 编译 ——
echo "【build】编译 DataHub ($MODE) ..."
(cd "$ROOT" && ./build.sh "$MODE_FLAG" DataHub)

EXE="$ROOT/build/$MODE/datahub"
if [[ ! -x "$EXE" ]]; then
    echo "【错误】编译产物不存在: $EXE"
    exit 1
fi

# —— ② 配置 Windows 端口转发（WSL2 环境）——
if [[ $IN_WSL -eq 1 ]]; then
    if [[ $DO_FORWARD -eq 1 ]]; then
        if command -v netsh.exe >/dev/null 2>&1; then
            WSL_IP="$(ip route get 1.1.1.1 2>/dev/null | grep -oP 'src \K[0-9.]+')"
            WSL_IP="${WSL_IP:-$(hostname -I | awk '{print $1}')}"
            echo "【forward】WSL IP = $WSL_IP，添加端口转发 Windows :$PORT → WSL $WSL_IP:$PORT"
            # 先删除旧规则（幂等），再添加
            netsh.exe interface portproxy delete v4tov4 listenport="$PORT" listenaddress=0.0.0.0 >/dev/null 2>&1 || true
            netsh.exe interface portproxy add v4tov4 listenport="$PORT" listenaddress=0.0.0.0 connectport="$PORT" connectaddress="$WSL_IP"
            # 放行 Windows 防火墙（幂等）
            powershell.exe -Command "if (-not (Get-NetFirewallRule -DisplayName 'DataHub $PORT' -ErrorAction SilentlyContinue)) { New-NetFirewallRule -DisplayName 'DataHub $PORT' -Direction Inbound -LocalPort $PORT -Protocol TCP -Action Allow | Out-Null }" >/dev/null 2>&1 || true
            echo "【forward】端口转发已配置。"
        else
            echo "【forward】未找到 netsh.exe，跳过端口转发（请手动在 Windows 配置）。"
        fi
    else
        echo "【forward】已跳过端口转发（--no-forward）。"
    fi
fi

# —— ③ 启动 ——
# 已存在则先停止（避免端口占用）
if pgrep -f "build/$MODE/datahub" >/dev/null 2>&1; then
    echo "【run】检测到旧实例，先停止 ..."
    pkill -f "build/$MODE/datahub" 2>/dev/null || true
    sleep 0.5
fi

echo "【run】启动 datahub，端口 $PORT ..."
cd "$DIR"
nohup "$EXE" "$PORT" >/tmp/datahub_$PORT.log 2>&1 &
sleep 1.2

# —— ④ 自检 ——
if ! pgrep -f "build/$MODE/datahub" >/dev/null 2>&1; then
    echo "【错误】datahub 启动失败，查看日志: /tmp/datahub_$PORT.log"
    exit 1
fi
echo "【run】datahub 已启动 (pid $(pgrep -f "build/$MODE/datahub" | head -1))"
echo "    日志: /tmp/datahub_$PORT.log"

# —— ⑤ 输出访问地址 ——
echo ""
echo "=========================================================="
echo " DataHub 访问地址"
echo "=========================================================="
echo "  本机:     http://127.0.0.1:$PORT/"
if [[ $IN_WSL -eq 1 ]]; then
    # ipconfig 中 IPv4 地址在"IPv4"标记的下一行（". . . : 192.168.x.x"）
    WLAN_IP="$(ipconfig.exe 2>/dev/null | strings | grep -A1 -iE '^\s*IPv4\s*$' \
        | grep -oP ':\s*\K[0-9.]+' \
        | grep -vE '^172\.28\.|^192\.168\.(217|147)\.' | head -1)"
    if [[ -n "$WLAN_IP" ]]; then
        echo "  局域网:   http://$WLAN_IP:$PORT/   (Windows 宿主机, 手机可访问)"
    fi
else
    LAN_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
    if [[ -n "$LAN_IP" ]]; then
        echo "  局域网:   http://$LAN_IP:$PORT/   (本机局域网 IP, 手机可访问)"
    fi
fi
echo "=========================================================="
echo ""
echo "提示: 停止服务用 ./run.sh --stop"
