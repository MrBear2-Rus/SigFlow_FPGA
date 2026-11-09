// 工程导出/导入：bundle 结构、复制、重复导入与非法包

import assert from "node:assert/strict";
import { rm } from "node:fs/promises";
import test from "node:test";

import { createApp } from "../src/http/server.js";
import { resolveToolPath } from "../src/util/config.js";
import { authedFetch, makeEnvironment, registerAndLogin } from "./helpers.js";

const TOOL = resolveToolPath({});

test("工程导出与导入", async () => {
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
            body: JSON.stringify({ projectId: "demo", template: "sample" }),
        });

        let response = await alice("/api/projects/demo/export");
        assert.equal(response.status, 200);
        const bundle = await response.json();
        assert.equal(bundle.schema, "sigflow.export.v1");
        assert.ok(bundle.files.some((file) => file.path === "src/top.v"));
        assert.ok(bundle.canvas && typeof bundle.canvas === "object");

        bundle.project.projectId = "copy";
        response = await alice("/api/projects/import", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify(bundle),
        });
        assert.equal(response.status, 201);
        assert.equal((await response.json()).files > 0, true);

        response = await alice("/api/projects/copy/file?path=src/top.v");
        assert.equal(response.status, 200);
        assert.match((await response.json()).content, /module top/);

        response = await alice("/api/projects/import", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify(bundle),
        });
        assert.equal(response.status, 409);

        response = await alice("/api/projects/import", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ schema: "other" }),
        });
        assert.equal(response.status, 400);
    } finally {
        await app.close();
        await rm(env.root, { recursive: true, force: true });
    }
});
