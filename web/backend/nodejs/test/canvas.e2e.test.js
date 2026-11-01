// =============================================================================
// J5 测试：画布读写与隔离
// =============================================================================

import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import { rm } from "node:fs/promises";
import test from "node:test";

import { createApp } from "../src/http/server.js";
import { resolveToolPath } from "../src/util/config.js";
import { authedFetch, makeEnvironment, registerAndLogin } from "./helpers.js";

const TOOL = resolveToolPath({});
const skip = existsSync(TOOL) ? false : `sigflow-tool 未构建：${TOOL}`;

const LAYOUT = {
    schema: "sigflow.canvas.v1",
    elements: [
        { id: "e1", type: "and", x: 120, y: 80, label: "and_1" },
        { id: "e2", type: "or", x: 320, y: 120, label: "or_1" },
    ],
    wires: [
        {
            id: "w1",
            from: { element: "e1", port: "out0", kind: "out", index: 0 },
            to: { element: "e2", port: "in0", kind: "in", index: 0 },
        },
    ],
};

test("画布：默认空布局、保存、读回、校验与隔离", { skip }, async () => {
    const env = await makeEnvironment();
    const app = createApp({
        toolPath: TOOL,
        databaseDir: env.databaseDir,
        userprojectsRoot: env.userprojectsRoot,
        toolRoot: env.toolsDir,
    });
    try {
        const address = await app.listen({ host: "127.0.0.1", port: 0 });
        const base = `http://127.0.0.1:${address.port}`;

        const cookie = await registerAndLogin(base);
        const alice = authedFetch(base, cookie);
        await alice("/api/projects", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ projectId: "demo" }),
        });

        // 未登录
        assert.equal((await fetch(`${base}/api/projects/demo/canvas`)).status, 401);

        // 默认空布局
        let response = await alice("/api/projects/demo/canvas");
        assert.equal(response.status, 200);
        const empty = await response.json();
        assert.deepEqual(empty.elements, []);
        assert.deepEqual(empty.wires, []);

        // 保存与读回
        response = await alice("/api/projects/demo/canvas", {
            method: "PUT",
            headers: { "content-type": "application/json" },
            body: JSON.stringify(LAYOUT),
        });
        assert.equal(response.status, 200);
        assert.deepEqual(await response.json(), { elements: 2, wires: 1 });

        response = await alice("/api/projects/demo/canvas");
        const saved = await response.json();
        assert.equal(saved.elements.length, 2);
        assert.equal(saved.wires.length, 1);
        assert.equal(saved.elements[0].type, "and");

        // 校验
        assert.equal(
            (
                await alice("/api/projects/demo/canvas", {
                    method: "PUT",
                    body: JSON.stringify({ elements: [] }),
                })
            ).status,
            400,
        );
        assert.equal(
            (await alice("/api/projects/demo/canvas", { method: "PUT", body: "[]" })).status,
            400,
        );

        // 隔离
        const bob = authedFetch(base, await registerAndLogin(base, "bob"));
        assert.equal((await bob("/api/projects/demo/canvas")).status, 404);

        // 静态资源
        assert.equal((await fetch(`${base}/pages/canvas.html`)).status, 200);
        assert.equal((await fetch(`${base}/scripts/canvas.js`)).status, 200);
        assert.equal((await fetch(`${base}/assets/canvas_elements.json`)).status, 200);
    } finally {
        await app.close();
        await rm(env.root, { recursive: true, force: true });
    }
});
