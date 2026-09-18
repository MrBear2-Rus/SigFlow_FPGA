#!/usr/bin/env bash
# tools/smoke_linux.sh
# Linux 启动冒烟/自检：验证 SigFlow 构建产物能正常启动，且无致命错误。
#
# 用法：
#   bash tools/smoke_linux.sh [sigflow可执行路径] [运行秒数]
#   例：bash tools/smoke_linux.sh build-linux/sigflow 20
#
# 退出码：0 = PASS，1 = FAIL
# 说明：有 $DISPLAY 时直接运行；否则用 xvfb-run 无头运行。已知环境噪声不判失败。
set -u

BIN="${1:-build-linux/sigflow}"
DUR="${2:-20}"
BIN="$(readlink -f "$BIN" 2>/dev/null || echo "$BIN")"
DIR="$(cd "$(dirname "$BIN")" 2>/dev/null && pwd || echo .)"
LOG="$(mktemp -t sigflow_smoke.XXXXXX.log)"

pass=0
fail=0
note() { printf '  [note] %s\n' "$*"; }
ok()   { printf '  [ OK ] %s\n' "$*"; pass=$((pass+1)); }
bad()  { printf '  [FAIL] %s\n' "$*"; fail=$((fail+1)); }

echo "=== SigFlow Linux smoke test ==="
echo "binary : $BIN"
echo "rundir : $DIR"
echo "seconds: $DUR"
echo

if [ ! -f "$BIN" ]; then bad "找不到可执行文件：$BIN"; echo; echo "RESULT: FAIL"; exit 1; fi
if [ ! -x "$BIN" ]; then bad "文件不可执行（chmod +x？）：$BIN"; fi
if command -v file >/dev/null 2>&1; then
    ft="$(file -b "$BIN")"
    echo "$ft" | grep -q "ELF" && ok "ELF 可执行" || bad "不是 ELF（$ft）"
fi

for res in "canvas_elements.json" "res/svg_icons/icon.svg" "res/icons/wiring.png"; do
    if [ -f "$DIR/$res" ]; then ok "资源存在：$res"
    else bad "缺少资源：$DIR/$res（重新构建以触发 POST_BUILD 拷贝）"; fi
done

if [ -n "${DISPLAY:-}" ]; then
    RUN=("$BIN")
else
    if command -v xvfb-run >/dev/null 2>&1; then
        RUN=(xvfb-run -a "$BIN"); note "无 DISPLAY，使用 xvfb-run 无头运行"
    else
        bad "无 DISPLAY 且未安装 xvfb-run（Arch: xorg-server-xvfb；Ubuntu: xvfb）"
        echo; echo "RESULT: FAIL"; exit 1
    fi
fi

cd "$HOME" 2>/dev/null || cd /
timeout "$DUR" "${RUN[@]}" > "$LOG" 2>&1
rc=$?
if [ "$rc" -eq 124 ]; then ok "运行 ${DUR}s 未崩溃（超时结束=正常）"
elif [ "$rc" -eq 0 ]; then ok "程序正常退出"
else bad "程序异常退出（rc=$rc）"; fi

BENIGN='dconf|cursor theme|GLib-GIO|GFileInfo|gtk/gfileinfo|duplicate image handler|lost focus|Gtk-WARNING|Gdk-Message'
FATAL='Segmentation|Aborted|terminate called|core dumped|assert.*failed|Assertion|undefined reference|fatal error'

if grep -qiE "$FATAL" "$LOG"; then
    bad "日志出现致命错误："
    grep -inE "$FATAL" "$LOG" | head -10 | sed 's/^/        /'
else
    ok "日志无致命错误"
fi

other="$(grep -inE 'error|not found|failed' "$LOG" | grep -viE "$BENIGN" | head -10)"
if [ -n "$other" ]; then
    note "日志中其它可疑行（请人工确认）："
    printf '%s\n' "$other" | sed 's/^/        /'
fi

if grep -q "JSON full path" "$LOG"; then
    jp="$(grep -m1 'JSON full path' "$LOG")"
    echo "$jp" | grep -qF "$DIR" && ok "资源路径按 exe 目录解析" \
        || note "资源路径不是 exe 目录：$jp"
fi

echo
echo "--- 日志尾部 ---"
tail -10 "$LOG" | sed 's/^/  /'
echo "--- 完整日志：$LOG ---"
echo
if [ "$fail" -eq 0 ]; then
    echo "RESULT: PASS  (checks ok=$pass, fail=$fail)"
    exit 0
fi
echo "RESULT: FAIL  (checks ok=$pass, fail=$fail)"
exit 1
