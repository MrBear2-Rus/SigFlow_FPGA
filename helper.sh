#!/usr/bin/env bash
# SigFlow openKylin 构建诊断脚本（修复版）
# 用法: ./sigflow_diag.sh [已有构建目录(可选)] [--local-wx|--system-wx]
# 输出: sigflow-diag-<时间戳>.txt （请回传该文件）
#
# 修复点（相对原版）:
#   1. 第 111 行续行 \ 后有空格导致语法错误 —— 改为一行
#   2. cmake 退出码用 PIPESTATUS[0] 取真实值
#   3. libcurl 兼容 libcurl / curl 两种 pkg-config 名
#   4. 构建日志完整落盘，不再只留 tail -120
#   5. SIGFLOW_USE_LOCAL_WX 不再写死 OFF，默认跟随项目默认，可用
#      --local-wx / --system-wx 显式指定
#   6. 新增 KARE 沙箱探测（openKylin 特有）
#   7. 新增 3rd/wxWidgets-src 存在性判断
#   8. 新增运行期库查找诊断（ldd / ldconfig）

set -u

REPO="$(pwd)"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$REPO/sigflow-diag-$STAMP.txt"
BUILD_DIR=""
WX_MODE="default"   # default | local | system

# ---- 参数解析 ----
while [ $# -gt 0 ]; do
  case "$1" in
    --local-wx)  WX_MODE="local";  shift ;;
    --system-wx) WX_MODE="system"; shift ;;
    -h|--help)
      echo "用法: $0 [已有构建目录] [--local-wx|--system-wx]"
      exit 0 ;;
    *)
      if [ -z "$BUILD_DIR" ]; then
        BUILD_DIR="$1"
      else
        echo "未知参数: $1" >&2; exit 1
      fi
      shift ;;
  esac
done
BUILD_DIR="${BUILD_DIR:-$REPO/build-diag}"

log() { echo "$@" | tee -a "$OUT"; }
sec() { log ""; log "==================== $* ===================="; }

log "SigFlow 构建诊断报告（修复版）"
log "生成时间: $STAMP"
log "仓库目录: $REPO"
log "构建目录: $BUILD_DIR"
log "wx 模式 : $WX_MODE"

# ---------------------------------------------------------------- 1. 系统信息
sec "1. 系统 / 架构"
{
  echo "uname: $(uname -a)"
  echo "--- /etc/os-release ---"; cat /etc/os-release 2>/dev/null
  echo "--- lsb_release ---"; lsb_release -a 2>/dev/null
  echo "--- 架构 ---"; uname -m; dpkg --print-architecture 2>/dev/null
  echo "--- CPU 核数 ---"; nproc
  echo "--- 内存 ---"; free -h 2>/dev/null
  echo "--- PATH ---"; echo "$PATH" | tr ':' '\n'
} | tee -a "$OUT"

# ---------------------------------------------------------------- 2. 工具链
sec "2. 工具链版本"
{
  echo "--- cmake ---"; command -v cmake && cmake --version 2>&1 | head -3
  echo "--- gcc/g++ ---"; gcc --version 2>&1 | head -1; g++ --version 2>&1 | head -1
  echo "--- make/ninja ---"; make --version 2>&1 | head -1; ninja --version 2>&1 | head -1
  echo "--- pkg-config ---"; command -v pkg-config && pkg-config --version 2>&1 | head -1
  echo "--- git ---"; command -v git && git --version 2>&1 | head -1
  echo "--- python3 ---"; python3 --version 2>&1 | head -1
} | tee -a "$OUT"

# ---------------------------------------------------------------- 3. 关键依赖
sec "3. 系统依赖（wxGTK / OpenGL / 其他）"
{
  echo "--- wxGTK pkg-config ---"
  for m in wxgtk3.2 wxgtk3.0-gtk3 wxgtk3.0 wxwidgets; do
    printf "%-18s: " "$m"; pkg-config --modversion "$m" 2>/dev/null || echo "(未找到)"
  done
  echo "--- wx-config ---"
  if command -v wx-config >/dev/null 2>&1; then
    wx-config --version 2>&1
    wx-config --libs 2>&1 | head -1
  else
    echo "(未找到)"
  fi
  echo "--- dpkg wx 包 ---"
  dpkg -l 2>/dev/null | grep -iE 'wxgtk|libwxbase|wxwidgets' | awk '{print $2, $3}' | head -20
  echo "--- 开发包探测 ---"
  for p in libwxgtk3.2-dev libwxgtk3.0-gtk3-dev libgtk-3-dev libgl1-mesa-dev libglu1-mesa-dev libcurl4-openssl-dev libftdi1-dev pkg-config; do
    if dpkg -s "$p" >/dev/null 2>&1; then echo "$p: 已安装"; else echo "$p: 未安装"; fi
  done
  echo "--- curl ---"
  for m in libcurl curl; do
    printf "%-10s: " "$m"; pkg-config --modversion "$m" 2>/dev/null || echo "(未找到)"
  done
} | tee -a "$OUT"

# ---------------------------------------------------------------- 3b. KARE 沙箱探测（openKylin 特有）
sec "3b. KARE 沙箱 / 非标准路径 wx 库"
{
  echo "--- /opt/kare 是否存在 ---"
  ls -ld /opt/kare 2>/dev/null || echo "(无 /opt/kare)"
  echo "--- KARE shadow 根 ---"
  ls -ld /var/opt/kare-applications 2>/dev/null || echo "(无 /var/opt/kare-applications)"
  echo "--- 查找 wx 共享库（全盘，可能较慢）---"
  find / -name 'libwx_gtk3u_core-3.2.so*' 2>/dev/null | head -20
  echo "--- ldconfig 缓存中的 wx ---"
  ldconfig -p 2>/dev/null | grep -i wx || echo "(ldconfig 缓存中无 wx)"
  echo "--- 标准路径 /usr/lib/x86_64-linux-gnu 下的 wx ---"
  ls -la /usr/lib/x86_64-linux-gnu/libwx_* 2>/dev/null | head -20 || echo "(标准路径无 wx)"
} | tee -a "$OUT"

# ---------------------------------------------------------------- 4. 仓库状态
sec "4. 仓库状态"
{
  echo "--- git ---"
  git -C "$REPO" rev-parse --abbrev-ref HEAD 2>/dev/null
  git -C "$REPO" log --oneline -1 2>/dev/null
  git -C "$REPO" status --short 2>/dev/null | head -20
  echo "--- FetchContent 相关 ---"
  grep -rn "FetchContent" "$REPO/CMakeLists.txt" "$REPO/cmake" 2>/dev/null || echo "(CMakeLists/cmake 无 FetchContent)"
  echo "--- wx 相关配置行 ---"
  grep -n "SIGFLOW_USE_LOCAL_WX\|SIGFLOW_WX_SRC\|SIGFLOW_WX_ROOT\|find_package(wxWidgets\|add_subdirectory.*WX" \
       "$REPO/CMakeLists.txt" 2>/dev/null
  echo "--- 源码目录是否存在 ---"
  for d in 3rd 3rd/wxWidgets-src 3rd/wxWidgets-3.2.9 3rd/httplib external external/fpga-tools; do
    if [ -e "$REPO/$d" ]; then echo "$d: 存在"; else echo "$d: 缺失"; fi
  done
  echo "--- 3rd/wxWidgets-src/CMakeLists.txt 是否存在（决定走本地编译 wx）---"
  if [ -f "$REPO/3rd/wxWidgets-src/CMakeLists.txt" ]; then
    echo "存在 → SIGFLOW_USE_LOCAL_WX=ON 时走本地源码编译"
  else
    echo "缺失 → 只能走系统 wx（find_package）"
  fi
  echo "--- 顶层目录 ---"; ls -1 "$REPO" | head -40
} | tee -a "$OUT"

# ---------------------------------------------------------------- 5. 全新配置
sec "5. 全新 CMake 配置（捕获完整报错）"
if [ "$BUILD_DIR" = "$REPO/build-diag" ]; then
  rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"

CMAKE_WX_ARG=()
case "$WX_MODE" in
  local)  CMAKE_WX_ARG=(-DSIGFLOW_USE_LOCAL_WX=ON) ;;
  system) CMAKE_WX_ARG=(-DSIGFLOW_USE_LOCAL_WX=OFF) ;;
  *)      CMAKE_WX_ARG=() ;;   # 跟随项目默认
esac

{
  echo "命令: cmake -S '$REPO' -B '$BUILD_DIR' -G 'Unix Makefiles' ${CMAKE_WX_ARG[*]}"
  echo "----------------------------------------"
  cmake -S "$REPO" -B "$BUILD_DIR" -G "Unix Makefiles" "${CMAKE_WX_ARG[@]}" 2>&1
  echo "----------------------------------------"
  echo "cmake 退出码: ${PIPESTATUS[0]}"
} | tee -a "$OUT"

# ---------------------------------------------------------------- 6. 若失败，再试系统 wx 探测
sec "6. 补充探测（find_package wxWidgets 行为）"
{
  TMP="$(mktemp -d)"
  cat > "$TMP/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.16)
project(wxprobe CXX)
set(wxWidgets_USE_STATIC_LIBS OFF)
find_package(wxWidgets COMPONENTS core base aui stc gl xml net QUIET)
if(wxWidgets_FOUND)
  message(STATUS "wxWidgets FOUND: ${wxWidgets_LIBRARIES}")
  message(STATUS "wxWidgets INCLUDE: ${wxWidgets_INCLUDE_DIRS}")
else()
  message(STATUS "wxWidgets NOT FOUND")
endif()
EOF
  cmake -S "$TMP" -B "$TMP/b" 2>&1
  echo "----------------------------------------"
  rm -rf "$TMP"
} | tee -a "$OUT"

# ---------------------------------------------------------------- 7. 构建
sec "7. 构建（若配置成功）"
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
  {
    echo "--- CMakeCache 中 wx 相关 ---"
    grep -iE 'SIGFLOW_USE_LOCAL_WX|SIGFLOW_WX|wxWidgets' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | head -20
    echo "--- 构建（完整日志）---"
    cmake --build "$BUILD_DIR" -j"$(nproc)" 2>&1
    echo "----------------------------------------"
    echo "build 退出码: ${PIPESTATUS[0]}"
  } | tee -a "$OUT"
else
  log "(跳过：配置未成功，无 CMakeCache.txt)"
fi

# ---------------------------------------------------------------- 8. 运行期诊断
sec "8. 运行期库查找诊断（关键：解释“编译过但跑不起来”）"
SIGFLOW_BIN="$BUILD_DIR/sigflow"
{
  if [ -x "$SIGFLOW_BIN" ]; then
    echo "--- ldd $SIGFLOW_BIN ---"
    ldd "$SIGFLOW_BIN" 2>&1 | grep -iE 'wx|not found' || ldd "$SIGFLOW_BIN" 2>&1 | head -40
    echo "--- 直接运行（仅取前 20 行错误）---"
    "$SIGFLOW_BIN" 2>&1 | head -20 || true
  else
    echo "(未找到可执行文件 $SIGFLOW_BIN)"
  fi
} | tee -a "$OUT"

# ---------------------------------------------------------------- 9. 关键文件快照
sec "9. 关键 CMake 片段"
{
  echo "===== CMakeLists.txt 前 120 行 ====="; sed -n '1,120p' "$REPO/CMakeLists.txt" 2>/dev/null
  echo "===== cmake/ 目录 ====="; ls -1 "$REPO/cmake" 2>/dev/null
  echo "===== cmake/toolchains（若有） ====="; ls -1 "$REPO/cmake/toolchains" 2>/dev/null
} | tee -a "$OUT"

sec "完成"
log "报告已写入: $OUT"
log "请把该文件（sigflow-diag-$STAMP.txt）回传。"
log ""
log "提示：若包含敏感路径，可按需替换用户名。"
