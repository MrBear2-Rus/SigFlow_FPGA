// 安全：CSRF 同源检查与登录限流

import assert from "node:assert/strict";
import { rm } from "node:fs/promises";
import test from "node:test";

import { createApp } from "../src/http/server.js";
import { resolveToolPath } from "../src/util/config.js";
import { makeEnvironment } from "./helpers.js";

const TOOL = resolveToolPath({});

test("CSRF：跨站 Origin 拒绝状态变更", async () => {
    const env = await makeEnvironment();
    const app = createApp({
        toolPath: TOOL,
        databaseDir: env.databaseDir,
        userprojectsRoot: env.userprojectsRoot,
    });
    try {
        const address = await app.listen({ host: "127.0.0.1", port: 0 });
        const base = `http://127.0.0.1:${address.port}`;
        await fetch(`${base}/api/users`, {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ userId: "alice", password: "password123" }),
        });

        let response = await fetch(`${base}/api/session`, {
            method: "POST",
            headers: { "content-type": "application/json", origin: "http://evil.example" },
            body: JSON.stringify({ userId: "alice", password: "password123" }),
        });
        assert.equal(response.status, 403);
        assert.equal((await response.json()).error.code, "CSRF_BLOCKED");

        response = await fetch(`${base}/api/session`, {
            method: "POST",
            headers: { "content-type": "application/json", origin: base },
            body: JSON.stringify({ userId: "alice", password: "password123" }),
        });
        assert.equal(response.status, 200);
    } finally {
        await app.close();
        await rm(env.root, { recursive: true, force: true });
    }
});

test("登录限流：5 次失败后锁定", async () => {
    const env = await makeEnvironment();
    const app = createApp({
        toolPath: TOOL,
        databaseDir: env.databaseDir,
        userprojectsRoot: env.userprojectsRoot,
    });
    try {
        const address = await app.listen({ host: "127.0.0.1", port: 0 });
        const base = `http://127.0.0.1:${address.port}`;
        await fetch(`${base}/api/users`, {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ userId: "alice", password: "password123" }),
        });

        const attempt = () =>
            fetch(`${base}/api/session`, {
                method: "POST",
                headers: { "content-type": "application/json" },
                body: JSON.stringify({ userId: "alice", password: "wrong-password" }),
            });

        for (let i = 0; i < 5; i += 1) assert.equal((await attempt()).status, 401);

        const blocked = await fetch(`${base}/api/session`, {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ userId: "alice", password: "password123" }),
        });
        assert.equal(blocked.status, 429);
        assert.ok((blocked.headers.get("retry-after") ?? "") !== "");
        assert.equal((await blocked.json()).error.code, "RATE_LIMITED");
    } finally {
        await app.close();
        await rm(env.root, { recursive: true, force: true });
    }
});
