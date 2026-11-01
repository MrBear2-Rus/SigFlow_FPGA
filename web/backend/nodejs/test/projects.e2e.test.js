// =============================================================================
// J3 集成测试：用户、会话、工程与文件
// =============================================================================

import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import { rm, symlink } from "node:fs/promises";
import { join } from "node:path";
import test from "node:test";

import { createApp } from "../src/http/server.js";
import { resolveToolPath } from "../src/util/config.js";
import { authedFetch, makeEnvironment, registerAndLogin } from "./helpers.js";

const TOOL = resolveToolPath({});
const skip = existsSync(TOOL) ? false : `sigflow-tool 未构建：${TOOL}`;

async function startApp() {
    const env = await makeEnvironment();
    const app = createApp({
        toolPath: TOOL,
        databaseDir: env.databaseDir,
        userprojectsRoot: env.userprojectsRoot,
        toolRoot: env.toolsDir,
    });
    const address = await app.listen({ host: "127.0.0.1", port: 0 });
    return { env, app, base: `http://127.0.0.1:${address.port}` };
}

test("用户：注册、重复注册、错误密码、未登录", { skip }, async () => {
    const { env, app, base } = await startApp();
    try {
        let response = await fetch(`${base}/api/users`, {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ userId: "alice", password: "password123" }),
        });
        assert.equal(response.status, 201);

        response = await fetch(`${base}/api/users`, {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ userId: "alice", password: "password123" }),
        });
        assert.equal(response.status, 409);

        response = await fetch(`${base}/api/users`, {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ userId: "bob", password: "short" }),
        });
        assert.equal(response.status, 400);

        response = await fetch(`${base}/api/session`, {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ userId: "alice", password: "wrong-password" }),
        });
        assert.equal(response.status, 401);

        assert.equal((await fetch(`${base}/api/projects`)).status, 401);

        const cookie = await registerAndLogin(base);
        const authed = authedFetch(base, cookie);
        response = await authed("/api/session");
        assert.equal((await response.json()).userId, "alice");
    } finally {
        await app.close();
        await rm(env.root, { recursive: true, force: true });
    }
});

test("工程与文件：创建、列表、读写、越界、隔离、删除", { skip }, async () => {
    const { env, app, base } = await startApp();
    try {
        const alice = authedFetch(base, await registerAndLogin(base, "alice"));
        const bob = authedFetch(base, await registerAndLogin(base, "bob"));

        let response = await alice("/api/projects", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ projectId: "demo", name: "演示工程" }),
        });
        assert.equal(response.status, 201);
        assert.equal((await response.json()).name, "演示工程");

        response = await alice("/api/projects");
        assert.equal((await response.json()).projects.length, 1);
        assert.equal((await bob("/api/projects")).status, 200);
        assert.equal((await (await bob("/api/projects")).json()).projects.length, 0);

        // 写入与读取
        const source = "module top(input a, output b);\n  assign b = a;\nendmodule\n";
        response = await alice("/api/projects/demo/file?path=src/top.v", {
            method: "PUT",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ content: source }),
        });
        assert.equal(response.status, 200);

        response = await alice("/api/projects/demo/file?path=src/top.v");
        assert.equal((await response.json()).content, source);

        response = await alice("/api/projects/demo/files");
        const tree = await response.json();
        assert.ok(tree.files.some((entry) => entry.path === "src/top.v" && entry.type === "file"));

        // 路径与类型限制
        assert.equal(
            (await alice("/api/projects/demo/file?path=../escape.v")).status,
            400,
        );
        assert.equal(
            (await alice("/api/projects/demo/file?path=src/top.exe")).status,
            400,
        );

        // 符号链接必须拒绝
        const linkPath = join(env.userprojectsRoot, "alice", "demo", "src", "link.v");
        await symlink("/etc/passwd", linkPath);
        assert.equal(
            (await alice("/api/projects/demo/file?path=src/link.v")).status,
            400,
        );

        // 工程隔离：bob 访问 alice 的工程
        assert.equal(
            (await bob("/api/projects/demo/file?path=src/top.v")).status,
            404,
        );

        // 元数据与删除
        response = await alice("/api/projects/demo");
        assert.equal((await response.json()).projectId, "demo");

        response = await alice("/api/projects/demo", { method: "DELETE" });
        assert.equal(response.status, 204);
        assert.equal((await (await alice("/api/projects")).json()).projects.length, 0);
    } finally {
        await app.close();
        await rm(env.root, { recursive: true, force: true });
    }
});
