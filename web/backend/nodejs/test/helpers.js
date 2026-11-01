// =============================================================================
// 测试辅助：临时工程、假 yosys、注册登录
// =============================================================================

import { chmod, mkdir, mkdtemp, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

/** 生成假 yosys：解析 .ys 里的 write_json 路径并写入最小合法网表。 */
export async function makeFakeYosys(dir, { sleepSeconds = 0 } = {}) {
    const path = join(dir, "yosys");
    const script = `#!/bin/sh
sleep ${sleepSeconds}
script=""
while [ $# -gt 0 ]; do
  case "$1" in
    -s) script="$2"; shift 2 ;;
    *) shift ;;
  esac
done
[ -n "$script" ] || exit 9
out=$(sed -n 's/^write_json "\\(.*\\)"$/\\1/p' "$script" | tail -n 1)
[ -n "$out" ] || exit 9
printf '{"modules":{"top":{"ports":{},"cells":{},"netnames":{}}}}' > "$out"
`;
    await writeFile(path, script);
    await chmod(path, 0o755);
    return path;
}

/** 建一个临时目录树：<root>/{db,projects,tools}，并准备假 yosys。 */
export async function makeEnvironment({ sleepSeconds = 0 } = {}) {
    const root = await mkdtemp(join(tmpdir(), "sigflow-j3-"));
    const toolsDir = join(root, "tools");
    await mkdir(toolsDir, { recursive: true });
    await makeFakeYosys(toolsDir, { sleepSeconds });
    return {
        root,
        toolsDir,
        databaseDir: join(root, "database"),
        userprojectsRoot: join(root, "userprojects"),
    };
}

/** 注册并登录，返回 Cookie 头的值。 */
export async function registerAndLogin(base, userId = "alice", password = "password123") {
    let response = await fetch(`${base}/api/users`, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ userId, password }),
    });
    if (response.status !== 201 && response.status !== 409) {
        throw new Error(`注册失败：HTTP ${response.status}`);
    }

    response = await fetch(`${base}/api/session`, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ userId, password }),
    });
    if (response.status !== 200) throw new Error(`登录失败：HTTP ${response.status}`);
    const cookie = (response.headers.get("set-cookie") ?? "").split(";")[0];
    if (!cookie) throw new Error("登录未返回 Cookie");
    return cookie;
}

/** 带 Cookie 的 fetch。 */
export function authedFetch(base, cookie) {
    return (path, options = {}) =>
        fetch(`${base}${path}`, {
            ...options,
            headers: { ...(options.headers ?? {}), cookie },
        });
}

export function synthParams(sourcePaths = ["src/top.v"]) {
    return {
        top_module: "top",
        source_files: sourcePaths,
        strategy: "baseline",
        yosys_family: "gw1n",
    };
}

export async function waitFor(predicate, timeoutMs = 5000) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        if (await predicate()) return;
        await new Promise((resolve) => setTimeout(resolve, 50));
    }
    throw new Error("等待超时");
}
