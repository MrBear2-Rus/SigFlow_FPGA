#!/usr/bin/env bash
# tools/build_verilator_msys2.sh
# 在 Windows 上用 MSYS2 + MinGW 源码编译 Verilator，并落位到
# external/fpga-tools/runtime/verilator（与 yosys/nextpnr/apicula 统一布局）。
#
# 为什么需要这个脚本：
#   FpgaTools.cmake 的 sigflow_verilator_build 目标需要在 MSYS2 里跑
#   （autoconf + ./configure 需要 sh / flex / bison / perl）。本脚本把同样的流程
#   独立出来，方便只想补 verilator 时单独构建。
#
# 先决条件（在 MSYS2 MINGW64 shell 里执行）：
#   pacman -Syu
#   pacman -S --needed base-devel mingw-w64-x86_64-gcc autoconf flex bison \
#       perl python git make
#
# 用法：
#   tools/build_verilator_msys2.sh
#
# 可覆盖的环境变量：
#   VERILATOR_REF=v5.052          要构建的 tag/commit（默认 v5.052）
#   VERILATOR_SRC_DIR=<dir>       源码目录（默认 <repo>/external/fpga-tools/src/verilator）
#   SIGFLOW_FPGA_RUNTIME_DIR=<dir> 落位目录（默认 <repo>/external/fpga-tools/runtime）
#   VERILATOR_JOBS=<n>            并行任务数（默认 nproc）
#   VERILATOR_FORCE=1             已有源码时仍重新 fetch/checkout
#
# 退出码：0 = 成功；非 0 = 失败
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

VERILATOR_REF="${VERILATOR_REF:-v5.052}"
VERILATOR_SRC_DIR="${VERILATOR_SRC_DIR:-$REPO_ROOT/external/fpga-tools/src/verilator}"
VERILATOR_TARBALL_URL="${VERILATOR_TARBALL_URL:-https://codeload.github.com/verilator/verilator/tar.gz/refs/tags/${VERILATOR_REF}}"
RUNTIME_DIR="${SIGFLOW_FPGA_RUNTIME_DIR:-$REPO_ROOT/external/fpga-tools/runtime}"
STAGE="$RUNTIME_DIR/verilator"
JOBS="${VERILATOR_JOBS:-$(nproc 2>/dev/null || echo 4)}"

ok()   { printf '  [ OK ] %s\n' "$*"; }
bad()  { printf '  [FAIL] %s\n' "$*"; }
note() { printf '  [note] %s\n' "$*"; }
die()  { bad "$*"; echo; echo "RESULT: FAIL"; exit 1; }

echo "=== Verilator (MSYS2/MinGW) 构建 ==="
echo "repo    : $REPO_ROOT"
echo "ref     : $VERILATOR_REF"
echo "src     : $VERILATOR_SRC_DIR"
echo "stage   : $STAGE"
echo "jobs    : $JOBS"
echo

case "${MSYSTEM:-}" in
    MINGW64) : ;;
    *) note "当前不是 MSYS2 MINGW64（MSYSTEM=${MSYSTEM:-unset}）；请在 “MSYS2 MINGW64” 终端里运行。" ;;
esac

missing=""
for t in gcc g++ make autoconf flex bison perl python3 git help2man; do
    command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
done
if [ -n "$missing" ]; then
    die "缺少构建工具：$missing
         请先执行：
         pacman -S --needed base-devel mingw-w64-x86_64-gcc autoconf flex bison perl python git make help2man"
fi
ok "构建工具齐全"

# MSYS2 没有 mingw-w64 版的 flex 包：FlexLexer.h 只在 /usr/include，
# 而 MinGW g++ 默认不搜索 /usr/include，直接编 V3ParseLex.cpp 会报
# "FlexLexer.h: No such file or directory"。这里把它拷进 mingw include（幂等）。
MINGW_INC="${MINGW_PREFIX:-/mingw64}/include"
if [ ! -f "$MINGW_INC/FlexLexer.h" ] && [ -f /usr/include/FlexLexer.h ]; then
    if cp /usr/include/FlexLexer.h "$MINGW_INC/FlexLexer.h" 2>/dev/null; then
        note "已拷贝 FlexLexer.h -> $MINGW_INC"
    else
        note "无法拷贝 FlexLexer.h 到 $MINGW_INC（若编译报缺该头，请手动处理）"
    fi
fi

# 有些网络环境下 git 协议（443）会被重置，而 codeload 的 tarball 可用 —— 提供回退。
fetch_tarball() {
    local tmp
    tmp="$(mktemp -d)"
    note "改用 tarball：$VERILATOR_TARBALL_URL"
    if ! curl -L --fail -o "$tmp/verilator.tar.gz" "$VERILATOR_TARBALL_URL"; then
        rm -rf "$tmp"; die "tarball 下载失败"
    fi
    rm -rf "$VERILATOR_SRC_DIR"
    mkdir -p "$VERILATOR_SRC_DIR"
    if ! tar -xzf "$tmp/verilator.tar.gz" -C "$VERILATOR_SRC_DIR" --strip-components=1; then
        rm -rf "$tmp"; die "tarball 解压失败"
    fi
    rm -rf "$tmp"
}

if [ -d "$VERILATOR_SRC_DIR/.git" ]; then
    if [ "${VERILATOR_FORCE:-0}" = "1" ]; then
        note "重新 fetch $VERILATOR_REF"
        git -C "$VERILATOR_SRC_DIR" fetch --tags --force origin || note "fetch 失败（沿用本地已有提交）"
    fi
    git -C "$VERILATOR_SRC_DIR" checkout "$VERILATOR_REF" || die "checkout $VERILATOR_REF 失败"
elif [ -f "$VERILATOR_SRC_DIR/configure.ac" ] && [ "${VERILATOR_FORCE:-0}" != "1" ]; then
    note "复用已有源码目录（非 git）：$VERILATOR_SRC_DIR"
else
    mkdir -p "$(dirname "$VERILATOR_SRC_DIR")"
    note "clone verilator 到 $VERILATOR_SRC_DIR"
    if ! git clone https://github.com/verilator/verilator "$VERILATOR_SRC_DIR"; then
        rm -rf "$VERILATOR_SRC_DIR"
        fetch_tarball
    fi
fi

# 生成 man 页需要 help2man；verilator.1 用 pod2man，而 MSYS2 的 pod2man 在
# /usr/bin/core_perl（默认不在 PATH），不补就会 "错误 127"。
export PATH="$PATH:/usr/bin/core_perl"

cd "$VERILATOR_SRC_DIR" || die "无法进入源码目录"

note "autoconf（生成 configure）"
autoconf || die "autoconf 失败"

note "configure --prefix=$STAGE"
./configure --prefix="$STAGE" || die "configure 失败"

note "make -j$JOBS（首次约 10–25 分钟）"
make -j"$JOBS" || die "编译失败"

note "make install"
make install || die "install 失败"

# 补齐 MinGW 运行时 DLL（verilator_bin 依赖 libstdc++/libgcc/libwinpthread）
MINGW_BIN="${MINGW_PREFIX:-/mingw64}/bin"
[ -d "$MINGW_BIN" ] || MINGW_BIN="$(dirname "$(command -v gcc)")"
for dll in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
    if [ -f "$MINGW_BIN/$dll" ]; then
        cp -f "$MINGW_BIN/$dll" "$STAGE/bin/" && note "已拷贝 $dll"
    fi
done

echo
FOUND=""
for b in verilator_bin_dbg.exe verilator_bin.exe verilator_bin_dbg verilator_bin; do
    if [ -f "$STAGE/bin/$b" ]; then FOUND="$STAGE/bin/$b"; break; fi
done
if [ -z "$FOUND" ]; then
    die "落位目录里找不到 verilator_bin：$STAGE/bin"
fi
ok "verilator 可执行：$FOUND"
if [ -d "$STAGE/share/verilator/include" ]; then
    ok "运行时头文件：$STAGE/share/verilator/include"
else
    bad "缺少 share/verilator/include（仿真编译会失败）"
fi

echo
echo "RESULT: PASS"
echo "verilator : $FOUND"
