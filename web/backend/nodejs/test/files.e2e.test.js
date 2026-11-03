// 文件移动与会话持久化

import assert from "node:assert/strict";
import { rm } from "node:fs/promises";
import test from "node:test";

import { createApp } from "../src/http/server.js";
import { resolveToolPath } from "../src/util/config.js";
import { authedFetch, makeEnvironment, registerAndLogin } from "./helpers.js";

const TOOL = resolveToolPath({});

test("文件移动：重命名、拒绝覆盖、拒绝越界", async () => {
    const env = await makeEnvironment();
    const app = createApp({
        toolPath: TOOL,
        databaseDir: env.databaseDir,
        userprojectsRoot: env.userprojectsRoot,
    });
    try {
        const address = await app.listen({ host: "127.0.0.1", port: 0 });
        const base = `http://127.0.0.1:${address.port}`;
        const alice = authedFetch(base, await registerAndLogin(base, "alice"));
        await alice("/api/projects", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ projectId: "demo" }),
        });
        const write = (path, content) =>
            alice(`/api/projects/demo/file?path=${encodeURIComponent(path)}`, {
                method: "PUT",
                headers: { "content-type": "application/json" },
                body: JSON.stringify({ content }),
            });

        assert.equal((await write("src/a.v", "module a; endmodule\n")).status, 200);
        assert.equal((await write("src/exists.v", "module b; endmodule\n")).status, 200);

        let response = await alice("/api/projects/demo/file/move", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ from: "src/a.v", to: "src/b.v" }),
        });
        assert.equal(response.status, 200);
        response = await alice("/api/projects/demo/file?path=src/b.v");
        assert.equal((await response.json()).content, "module a; endmodule\n");
        assert.equal((await alice("/api/projects/demo/file?path=src/a.v")).status, 404);

        response = await alice("/api/projects/demo/file/move", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ from: "src/b.v", to: "src/exists.v" }),
        });
        assert.equal(response.status, 409);

        response = await alice("/api/projects/demo/file/move", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ from: "src/b.v", to: "../escape.v" }),
        });
        assert.equal(response.status, 400);
        assert.equal((await alice("/api/projects/demo/file?path=src/b.v")).status, 200);
    } finally {
        await app.close();
        await rm(env.root, { recursive: true, force: true });
    }
});

test("会话持久化：重启后旧 Cookie 仍有效，登出后失效", async () => {
    const env = await makeEnvironment();
    const options = {
        toolPath: TOOL,
        databaseDir: env.databaseDir,
        userprojectsRoot: env.userprojectsRoot,
    };
    try {
        const first = createApp(options);
        const address = await first.listen({ host: "127.0.0.1", port: 0 });
        const base = `http://127.0.0.1:${address.port}`;
        const cookie = await registerAndLogin(base, "alice");
        await first.close();

        const second = createApp(options);
        const next = await second.listen({ host: "127.0.0.1", port: 0 });
        const nextBase = `http://127.0.0.1:${next.port}`;
        let response = await fetch(`${nextBase}/api/session`, { headers: { cookie } });
        assert.equal(response.status, 200);
        assert.equal((await response.json()).userId, "alice");

        response = await fetch(`${nextBase}/api/session`, {
            method: "DELETE",
            headers: { cookie },
        });
        assert.equal(response.status, 204);
        await second.close();

        const third = createApp(options);
        const last = await third.listen({ host: "127.0.0.1", port: 0 });
        response = await fetch(`http://127.0.0.1:${last.port}/api/session`, { headers: { cookie } });
        assert.equal(response.status, 401);
        await third.close();
    } finally {
        await rm(env.root, { recursive: true, force: true });
    }
});
