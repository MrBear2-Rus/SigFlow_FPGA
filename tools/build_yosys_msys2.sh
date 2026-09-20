#!/usr/bin/env bash
# tools/build_yosys_msys2.sh
# 在 Windows 上用 MSYS2 + MinGW 源码编译一个**完整的** yosys（含 connect pass），
# 并落位到 external/fpga-tools/runtime/yosys。
#
# 背景
#   Yosys 官方只在 oss-cad-suite（整套，Windows 包 ~316MB）里发 Windows 二进制，
#   没有 yosys-only 的发布件；conda-forge 的 yosys 没有 win-64；Yosys 不支持 MSVC。
#   而随包分发的 runtime/yosys 若是第三方 MSVC 残缺构建，会缺 `connect` 等命令 ——
#   TraceBridge 的调试位流脚本用 `flatten` + `connect -set ...` 绑定内部探针，
#   一旦缺 connect 就直接 `ERROR: No such command: connect`，Yosys 阶段失败。
#   本脚本编完后会**自动校验 `help connect`**，不通过就报错退出。
#
# 先决条件（在 MSYS2 MINGW64 shell 里执行）：
#   pacman -Syu
#   pacman -S --needed base-devel mingw-w64-x86_64-gcc bison flex git \
#       make pkg-config tcl libffi libreadline zlib
#
# 用法：
#   tools/build_yosys_msys2.sh
#
# 可覆盖的环境变量：
#   YOSYS_REF=v0.49        要构建的 tag/commit（默认 v0.49；建议与 cmake 里对齐）
#   YOSYS_SRC_DIR=<dir>    源码目录（默认 <repo>/external/fpga-tools/src/yosys）
#   SIGFLOW_FPGA_RUNTIME_DIR=<dir>  落位目录（默认 <repo>/external/fpga-tools/runtime）
#   YOSYS_JOBS=<n>         并行任务数（默认 nproc）
#   YOSYS_FORCE=1          已有源码时仍重新 fetch/checkout
#
# 退出码：0 = 成功且 connect 可用；非 0 = 失败
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

YOSYS_REF="${YOSYS_REF:-v0.49}"
YOSYS_SRC_DIR="${YOSYS_SRC_DIR:-$REPO_ROOT/external/fpga-tools/src/yosys}"
RUNTIME_DIR="${SIGFLOW_FPGA_RUNTIME_DIR:-$REPO_ROOT/external/fpga-tools/runtime}"
STAGE="$RUNTIME_DIR/yosys"
JOBS="${YOSYS_JOBS:-$(nproc 2>/dev/null || echo 4)}"

ok()   { printf '  [ OK ] %s\n' "$*"; }
bad()  { printf '  [FAIL] %s\n' "$*"; }
note() { printf '  [note] %s\n' "$*"; }
die()  { bad "$*"; echo; echo "RESULT: FAIL"; exit 1; }

# yosys.exe 是否支持 connect（用 `help connect` 正文里的独有句子判断；
# 注意 "create or remove connections" 只出现在 `help` 的命令列表里，不在 `help connect` 正文）
has_connect() {
    local bin="$1"
    [ -f "$bin" ] || return 1
    "$bin" -Q -p "help connect" 2>&1 | grep -q "Unconnect all existing drivers"
}

echo "=== Yosys (MSYS2/MinGW) 完整构建 ==="
echo "repo    : $REPO_ROOT"
echo "ref     : $YOSYS_REF"
echo "src     : $YOSYS_SRC_DIR"
echo "stage   : $STAGE"
echo "jobs    : $JOBS"
echo

# --- 环境检查 ---
case "${MSYSTEM:-}" in
    MINGW64) : ;;
    *) note "当前不是 MSYS2 MINGW64（MSYSTEM=${MSYSTEM:-unset}）；请在 “MSYS2 MINGW64” 终端里运行。" ;;
esac

missing=""
for t in gcc g++ make bison flex git python3; do
    command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
done
if [ -n "$missing" ]; then
    die "缺少构建工具：$missing
         请先执行：
         pacman -S --needed base-devel mingw-w64-x86_64-gcc bison flex git make pkg-config python \
             tcl libffi libreadline zlib"
fi
ok "构建工具齐全"

# --- 取源码（含 abc 子模块） ---
if [ -d "$YOSYS_SRC_DIR/.git" ]; then
    if [ "${YOSYS_FORCE:-0}" = "1" ]; then
        note "重新 fetch $YOSYS_REF"
        git -C "$YOSYS_SRC_DIR" fetch --tags --force origin || note "fetch 失败（沿用本地已有提交）"
    fi
else
    mkdir -p "$(dirname "$YOSYS_SRC_DIR")"
    note "clone yosys 到 $YOSYS_SRC_DIR"
    git clone https://github.com/YosysHQ/yosys "$YOSYS_SRC_DIR" || die "clone 失败（网络？）"
fi

git -C "$YOSYS_SRC_DIR" checkout "$YOSYS_REF" || die "checkout $YOSYS_REF 失败"

note "更新子模块（abc/cxxopts，可能较慢）"
git -C "$YOSYS_SRC_DIR" submodule update --init --recursive || die "submodule 更新失败"
[ -f "$YOSYS_SRC_DIR/abc/Makefile" ] || die "abc 子模块缺失：$YOSYS_SRC_DIR/abc"

# abc 在 _WIN32 下会同时 include 自带的 "lib/pthread.h"（pthreads-win32，原本给 MSVC）
# 与系统 <pthread.h>。MinGW 用 winpthreads，两者声明冲突
# （"conflicting declaration of pthread_barrierattr_init..."）。
# 把自带头统一替换成系统 <pthread.h>；幂等，只会生效一次。
note "修补 abc 的 pthread 头引用（MinGW/winpthreads）"
while IFS= read -r abc_src; do
    sed -i 's#"\.\./lib/pthread\.h"#<pthread.h>#' "$abc_src"
done < <(grep -rl 'lib/pthread\.h' "$YOSYS_SRC_DIR/abc/src" 2>/dev/null || true)

# --- 备份旧的残缺 yosys ---
if [ -f "$STAGE/bin/yosys.exe" ] && ! has_connect "$STAGE/bin/yosys.exe"; then
    BACKUP="$STAGE/bin/yosys.exe.missing-connect.bak"
    if mv -f "$STAGE/bin/yosys.exe" "$BACKUP" 2>/dev/null; then
        note "旧 yosys.exe 不含 connect，已备份为 $(basename "$BACKUP")"
    fi
fi

# --- 构建 ---
cd "$YOSYS_SRC_DIR" || die "无法进入源码目录"

# 关闭 TCL / readline / plugins：避免额外 DLL 依赖（只需 MinGW 运行时 DLL）。
# ★ 这些变量必须传给**每一次 make**：config-* 会把探测结果写进 Makefile.conf，
#   只在 config 时传会被覆盖，build 阶段仍会去找 readline/tcl 头。
# 注：不要设 DISABLE_ABC_THREADS=1 —— abc 的 wlcPth.c 在 NO_PTHREADS 分支里仍引用
#     `abc::g_mutex`，会链接失败；线程头冲突已由上面的 abc 源码补丁解决。
YOSYS_MAKE_FLAGS=(ENABLE_TCL=0 ENABLE_READLINE=0 ENABLE_PLUGINS=0)

if [ "${YOSYS_FORCE:-0}" = "1" ] || ! grep -q '^CONFIG := msys2-64' Makefile.conf 2>/dev/null; then
    note "make config-msys2-64（ENABLE_TCL=0 ENABLE_READLINE=0 ENABLE_PLUGINS=0）"
    if ! make config-msys2-64 "${YOSYS_MAKE_FLAGS[@]}"; then
        note "config-msys2-64 不可用，回退 config-gcc"
        make config-gcc "${YOSYS_MAKE_FLAGS[@]}" || die "config 失败"
    fi
else
    note "Makefile.conf 已是 msys2-64，跳过 config（YOSYS_FORCE=1 可强制重配）"
fi

note "make -j$JOBS（首次约 5–15 分钟）"
make -j"$JOBS" "${YOSYS_MAKE_FLAGS[@]}" || die "编译失败"

note "make install PREFIX=$STAGE"
make install PREFIX="$STAGE" "${YOSYS_MAKE_FLAGS[@]}" || die "install 失败"

# --- 补齐 MinGW 运行时 DLL ---
MINGW_BIN="${MINGW_PREFIX:-/mingw64}/bin"
[ -d "$MINGW_BIN" ] || MINGW_BIN="$(dirname "$(command -v gcc)")"
for dll in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
    if [ -f "$MINGW_BIN/$dll" ]; then
        cp -f "$MINGW_BIN/$dll" "$STAGE/bin/" && note "已拷贝 $dll"
    fi
done

# --- 校验 ---
echo
if has_connect "$STAGE/bin/yosys.exe"; then
    ok "yosys 完整：connect 可用"
else
    die "生成的 yosys 仍不支持 connect（$STAGE/bin/yosys.exe）"
fi
"$STAGE/bin/yosys.exe" -V 2>&1 | sed 's/^/  /'
if [ -f "$STAGE/bin/yosys-abc.exe" ]; then ok "yosys-abc.exe 存在"; else bad "缺少 yosys-abc.exe"; fi

echo
echo "RESULT: PASS"
echo "yosys : $STAGE/bin/yosys.exe"
