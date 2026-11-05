#!/usr/bin/env bash
# =============================================================================
# 部署 web/ 到 maomio.top 服务器（SigFlow）
#
#   ./deploy.sh                    # 默认部署到 root@maomio.top:/root/web
#   DEPLOY_PASSWORD=xxx ./deploy.sh   # 非交互（密码认证）
#   DRY_RUN=1 ./deploy.sh             # 只演练，不改远端
#   BUILD=0 ./deploy.sh               # 不重新编译 C++，只更新 web 层
#
# 流程：
#   1. 本地 zip 打包 web/，排除 build/、运行期数据、缓存、deploy.sh 本身；
#   2. scp -r 上传到远端 /tmp；
#   3. 远端 unzip 到临时目录，再用 rsync 覆盖目标目录：
#        --delete 清掉远端多余旧文件（也就是“替换掉远程的”）；
#        但排除 build/、backend/database、backend/userprojects、config.txt，
#        这四项在远端原地保留，数据一个字节都不动；
#   4. 远端 cmake 重新构建（BUILD=1 时）；
#   5. systemctl restart sigflow，并轮询 /api/health。
#
# 关键环境变量（都有默认值，只有换机器时才需要设）：
#   DEPLOY_REMOTE=root@maomio.top   远端地址
#   DEPLOY_PATH=/root/web           远端 web 目录
#   DEPLOY_PORT=22                  SSH 端口
#   SIGFLOW_SERVICE=sigflow         systemd 服务名（空则跳过重启）
#   HEALTH_URL=http://127.0.0.1:18080/api/health
#   BUILD=1                         1=远端重建 C++，0=跳过
#   PRESERVE_CONFIG=1               1=保留远端 config.txt（默认），0=用本地覆盖
#   JOBS=<nproc>                    cmake 并行度
#   KEEP_ZIP=0                      1=保留本地 zip
#   DRY_RUN=0                       1=只打印会变动的文件，不做任何修改
#   DEPLOY_PASSWORD                 ssh 密码（不设则走交互/密钥）
#   DEPLOY_SSH_CONFIG=/dev/null     本机 /etc/ssh/ssh_config.d 权限异常，默认不吃系统配置
# =============================================================================

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

DEPLOY_REMOTE="${DEPLOY_REMOTE:-root@maomio.top}"
DEPLOY_PATH="${DEPLOY_PATH:-/root/web}"
DEPLOY_PORT="${DEPLOY_PORT:-22}"
SIGFLOW_SERVICE="${SIGFLOW_SERVICE:-sigflow}"
HEALTH_URL="${HEALTH_URL:-http://127.0.0.1:18080/api/health}"
BUILD="${BUILD:-1}"
PRESERVE_CONFIG="${PRESERVE_CONFIG:-1}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
# 服务器是云盘小机器：并行度过高会全部卡在 D 状态（I/O 等待），反而更慢
if [ "$JOBS" -gt 8 ]; then JOBS=8; fi
KEEP_ZIP="${KEEP_ZIP:-0}"
DRY_RUN="${DRY_RUN:-0}"

for cmd in zip scp ssh; do
    command -v "$cmd" >/dev/null 2>&1 || {
        echo "[-] 缺少本地命令：$cmd" >&2
        exit 1
    }
done

STAMP="$(date +%Y%m%d-%H%M%S)"
ZIP_LOCAL="${TMPDIR:-/tmp}/sigflow-web-${STAMP}.zip"
REMOTE_ZIP="/tmp/sigflow-web-${STAMP}.zip"
REMOTE_STAGING="/tmp/sigflow-web-${STAMP}.d"

# ---- ssh/scp 公共参数 -------------------------------------------------------
SSH_OPTS=(
    -F "${DEPLOY_SSH_CONFIG:-/dev/null}"
    -o StrictHostKeyChecking=accept-new
    -o UserKnownHostsFile="${HOME}/.ssh/known_hosts"
    -o LogLevel=ERROR
    -p "$DEPLOY_PORT"
)
SCP_OPTS=(
    -r
    -F "${DEPLOY_SSH_CONFIG:-/dev/null}"
    -o StrictHostKeyChecking=accept-new
    -o UserKnownHostsFile="${HOME}/.ssh/known_hosts"
    -o LogLevel=ERROR
    -P "$DEPLOY_PORT"
)

# 密码认证：用一次性 askpass 脚本，避免依赖 sshpass
ASKPASS_FILE=""
if [ -n "${DEPLOY_PASSWORD:-}" ]; then
    ASKPASS_FILE="$(mktemp)"
    PW_B64="$(printf '%s' "$DEPLOY_PASSWORD" | base64 | tr -d '\n')"
    {
        echo '#!/bin/sh'
        echo "echo '${PW_B64}' | base64 -d"
    } >"$ASKPASS_FILE"
    chmod 700 "$ASKPASS_FILE"
    export SSH_ASKPASS="$ASKPASS_FILE"
    export SSH_ASKPASS_REQUIRE=force
    export DISPLAY="${DISPLAY:-:0}"
    SSH_OPTS+=(-o PreferredAuthentications=password -o PubkeyAuthentication=no -o NumberOfPasswordPrompts=1)
    SCP_OPTS+=(-o PreferredAuthentications=password -o PubkeyAuthentication=no -o NumberOfPasswordPrompts=1)
fi

do_ssh() {
    if [ -n "$ASKPASS_FILE" ]; then
        setsid ssh "${SSH_OPTS[@]}" "$@"
    else
        ssh "${SSH_OPTS[@]}" "$@"
    fi
}

cleanup_local() {
    [ -n "$ASKPASS_FILE" ] && rm -f "$ASKPASS_FILE"
    if [ "$KEEP_ZIP" = "1" ]; then
        [ -n "${ZIP_LOCAL:-}" ] && [ -f "$ZIP_LOCAL" ] && echo "[i] 保留本地包：$ZIP_LOCAL"
    else
        [ -n "${ZIP_LOCAL:-}" ] && rm -f "$ZIP_LOCAL"
    fi
}
trap cleanup_local EXIT

# ---- 1. 打包 ---------------------------------------------------------------
echo "==> [1/5] 打包 web/（排除 build、运行期数据与缓存）"
EXCLUDES=(
    "build/*"
    "build-*/*"
    ".git/*"
    ".shots/*"
    ".pytest_cache/*"
    "node_modules/*"
    "*/__pycache__/*"
    "*.zip"
    "*.pyc"
    ".DS_Store"
    "deploy.sh"
    "backend/database/*"
    "backend/userprojects/*"
)
if [ "$PRESERVE_CONFIG" = "1" ]; then
    EXCLUDES+=("config.txt")
fi

ZIP_ARGS=()
for pattern in "${EXCLUDES[@]}"; do
    ZIP_ARGS+=(-x "$pattern")
done

rm -f "$ZIP_LOCAL"
zip -qr "$ZIP_LOCAL" . "${ZIP_ARGS[@]}"
echo "    包大小：$(du -h "$ZIP_LOCAL" | cut -f1)  （$ZIP_LOCAL）"

# ---- 2. 上传 ---------------------------------------------------------------
echo "==> [2/5] scp 上传到 ${DEPLOY_REMOTE}:${REMOTE_ZIP}"
if [ -n "$ASKPASS_FILE" ]; then
    setsid scp "${SCP_OPTS[@]}" "$ZIP_LOCAL" "${DEPLOY_REMOTE}:${REMOTE_ZIP}"
else
    scp "${SCP_OPTS[@]}" "$ZIP_LOCAL" "${DEPLOY_REMOTE}:${REMOTE_ZIP}"
fi

# ---- 3~5. 远端解压、替换、构建、重启 ---------------------------------------
echo "==> [3/5] 远端解压与替换（保留 build/、backend/database、backend/userprojects）"
do_ssh "$DEPLOY_REMOTE" \
    DEPLOY_PATH="$DEPLOY_PATH" \
    REMOTE_ZIP="$REMOTE_ZIP" \
    REMOTE_STAGING="$REMOTE_STAGING" \
    PRESERVE_CONFIG="$PRESERVE_CONFIG" \
    BUILD="$BUILD" \
    JOBS="$JOBS" \
    DRY_RUN="$DRY_RUN" \
    SIGFLOW_SERVICE="$SIGFLOW_SERVICE" \
    HEALTH_URL="$HEALTH_URL" \
    'bash -s' <<'REMOTE'
set -euo pipefail

case "${DEPLOY_PATH:?}" in
    / | "" ) echo "[-] DEPLOY_PATH 不安全：$DEPLOY_PATH" >&2; exit 1 ;;
esac
[ -d "$DEPLOY_PATH/backend/nodejs" ] || {
    echo "[-] 目标目录不像 SigFlow web：$DEPLOY_PATH" >&2
    exit 1
}
command -v unzip >/dev/null 2>&1 || { echo "[-] 远端缺少 unzip" >&2; exit 1; }
command -v rsync >/dev/null 2>&1 || { echo "[-] 远端缺少 rsync" >&2; exit 1; }

STAGING="$REMOTE_STAGING"
cleanup_remote() {
    rm -rf "$STAGING" "$REMOTE_ZIP"
}
trap cleanup_remote EXIT

# 数据指纹：部署前后必须完全一致
DATA_REL=("backend/database" "backend/userprojects")
fingerprint() {
    for rel in "${DATA_REL[@]}"; do
        [ -d "$DEPLOY_PATH/$rel" ] || { echo "  $rel: (不存在)"; continue; }
        n=$(find "$DEPLOY_PATH/$rel" -type f | wc -l)
        h=$(find "$DEPLOY_PATH/$rel" -type f -print0 | sort -z | xargs -0 -r sha256sum | sha256sum | cut -d' ' -f1)
        echo "  $rel: ${n} 个文件, 内容指纹 ${h:0:16}"
    done
}

echo "    部署前远端数据："
fingerprint

rm -rf "$STAGING"
mkdir -p "$STAGING"
unzip -q "$REMOTE_ZIP" -d "$STAGING"

RSYNC_ARGS=(
    -a
    --delete
    --itemize-changes
    --exclude '/build'
    --exclude '/build-*'
    --exclude '/.git'
    --exclude '/backend/database'
    --exclude '/backend/userprojects'
)
if [ "$PRESERVE_CONFIG" = "1" ]; then
    RSYNC_ARGS+=(--exclude '/config.txt')
else
    echo "    （PRESERVE_CONFIG=0：将用本地 config.txt 覆盖远端）"
fi
[ "$DRY_RUN" = "1" ] && RSYNC_ARGS+=(-n)

CHANGE_LIST=$(rsync "${RSYNC_ARGS[@]}" "$STAGING/" "$DEPLOY_PATH/" || true)
CHANGES=$(printf '%s' "$CHANGE_LIST" | grep -c '^' || true)
echo "    待变更条目：$CHANGES"

if [ "$DRY_RUN" = "1" ]; then
    if [ "$CHANGES" -gt 0 ]; then
        echo "    变更预览（前 30 行）："
        printf '%s\n' "$CHANGE_LIST" | head -30 | sed 's/^/      /'
    fi
    echo "    [dry-run] 以上为 rsync 计划，未做任何修改；跳过构建与重启"
    exit 0
fi

[ -x "$DEPLOY_PATH/build/bin/sigflow-tool" ] || {
    echo "[-] 远端 build/bin/sigflow-tool 丢失，部署中止（请检查 build 是否被误删）" >&2
    exit 1
}

# 本次是否动了 C++ 源码（决定要不要重编）
CPP_CHANGED=0
NONTRIVIAL=$(printf '%s\n' "$CHANGE_LIST" | grep -E '^[<>c*]' || true)
if [ -n "$NONTRIVIAL" ] && printf '%s\n' "$NONTRIVIAL" | grep -qE 'backend/cpp/|(^|[ /])CMakeLists\.txt'; then
    CPP_CHANGED=1
fi

if [ "${BUILD:-1}" != "1" ]; then
    echo "    跳过构建（BUILD=0）"
    if [ "$CPP_CHANGED" = "1" ]; then
        echo "    [!] 本次有 C++ 源码变更，但 BUILD=0，远端二进制仍是旧的"
    fi
elif [ "$CPP_CHANGED" = "0" ] && [ -f "$DEPLOY_PATH/build/CMakeCache.txt" ] && [ -x "$DEPLOY_PATH/build/bin/sigflow-tool" ]; then
    echo "    C++ 源码无变更，跳过构建"
else
    # 这台机器只有 2GB 内存，并行度按可用内存收敛，否则全卡在 D 状态互相抢 swap
    AVAIL_MB=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo)
    LIM=$(( AVAIL_MB / 800 ))
    [ "$LIM" -lt 1 ] && LIM=1
    [ "$LIM" -gt 8 ] && LIM=8
    if [ "$JOBS" -gt "$LIM" ]; then
        JOBS=$LIM
        echo "    可用内存 ${AVAIL_MB}MB，并行度收敛为 -j ${JOBS}"
    fi
    echo "    重新构建 C++（-j ${JOBS}）…"
    # 只在没有构建缓存时才配置；已有缓存时重复传 -DCMAKE_BUILD_TYPE 会改编译选项，
    # 触发全量重编。
    if [ ! -f "$DEPLOY_PATH/build/CMakeCache.txt" ]; then
        echo "    未发现 CMakeCache，首次配置…"
        cmake -S "$DEPLOY_PATH" -B "$DEPLOY_PATH/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
    fi
    cmake --build "$DEPLOY_PATH/build" -j "$JOBS"
fi

if [ -n "${SIGFLOW_SERVICE:-}" ]; then
    echo "    重启服务：$SIGFLOW_SERVICE"
    systemctl restart "$SIGFLOW_SERVICE"
    sleep 2
    systemctl is-active --quiet "$SIGFLOW_SERVICE" || {
        echo "[-] $SIGFLOW_SERVICE 未起来，查看：journalctl -u $SIGFLOW_SERVICE -n 50" >&2
        exit 1
    }
else
    echo "    SIGFLOW_SERVICE 为空，未重启服务"
fi

echo -n "    健康检查 $HEALTH_URL ："
for _ in $(seq 1 30); do
    if curl -fsS "$HEALTH_URL" >/dev/null 2>&1; then
        echo "OK"
        break
    fi
    sleep 1
done
curl -fsS "$HEALTH_URL" >/dev/null 2>&1 || echo "未通过（服务可能仍在启动，或端口与 config.txt 不符）"

echo "    部署后远端数据："
fingerprint
REMOTE

echo "==> [4/5] 完成文件替换与服务重启"
echo "==> [5/5] 部署完成：${DEPLOY_REMOTE}:${DEPLOY_PATH}"
