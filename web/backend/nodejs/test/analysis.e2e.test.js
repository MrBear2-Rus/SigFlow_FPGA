// =============================================================================
// 结构树 / 诊断接口
//
// C++ 已实现 structure / lint（contracts/analysis-v1）；lint 需要工程根，
// 由 Node 自动传入，测试验证真实结果与路径校验。
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

test("分析与诊断接口：真实结果、相对路径与校验", { skip }, async () => {
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

        const authed = authedFetch(base, await registerAndLogin(base));
        await authed("/api/projects", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ projectId: "demo" }),
        });
        await authed("/api/projects/demo/file?path=src/top.v", {
            method: "PUT",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ content: "module top; endmodule\n" }),
        });

        // 未登录
        assert.equal(
            (
                await fetch(`${base}/api/projects/demo/structure`, {
                    method: "POST",
                    body: JSON.stringify({ path: "src/top.v" }),
                })
            ).status,
            401,
        );

        // structure：真实节点树
        let response = await authed("/api/projects/demo/structure", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ path: "src/top.v" }),
        });
        assert.equal(response.status, 200);
        const tree = await response.json();
        assert.equal(tree.schema, "sigflow.structure.v1");
        assert.ok(Array.isArray(tree.nodes) && tree.nodes.length > 0);
        const module = tree.nodes.find((node) => node.kind === "module");
        assert.deepEqual(module?.name, "top");
        assert.ok(module.range.start.line >= 1 && module.range.start.col >= 1);

        // lint：需要工程根（Node 自动传），空诊断
        response = await authed("/api/projects/demo/lint", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ path: "src/top.v" }),
        });
        assert.equal(response.status, 200);
        const lint = await response.json();
        assert.equal(lint.schema, "sigflow.lint.v1");
        assert.ok(Array.isArray(lint.diagnostics));

        for (const endpoint of ["structure", "lint"]) {
            // 文件不存在
            assert.equal(
                (
                    await authed(`/api/projects/demo/${endpoint}`, {
                        method: "POST",
                        body: JSON.stringify({ path: "src/nope.v" }),
                    })
                ).status,
                404,
            );

            // 路径越界
            assert.equal(
                (
                    await authed(`/api/projects/demo/${endpoint}`, {
                        method: "POST",
                        body: JSON.stringify({ path: "../escape.v" }),
                    })
                ).status,
                400,
            );
        }
    } finally {
        await app.close();
        await rm(env.root, { recursive: true, force: true });
    }
});
