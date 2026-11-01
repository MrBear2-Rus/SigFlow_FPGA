// =============================================================================
// J4 测试：结构树 / 诊断接口
//
// C++ 侧 P6 尚未实现，接口当前返回 501；已实现后应返回 200。
// 两种结果都接受，保证 C++ 落地后测试不用改。
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

test("分析与诊断接口：未实现返回 501，路径校验生效", { skip }, async () => {
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

        for (const endpoint of ["structure", "lint"]) {
            const response = await authed(`/api/projects/demo/${endpoint}`, {
                method: "POST",
                headers: { "content-type": "application/json" },
                body: JSON.stringify({ path: "src/top.v" }),
            });
            assert.ok(
                response.status === 200 || response.status === 501,
                `${endpoint} 返回 ${response.status}`,
            );
            if (response.status === 501) {
                assert.equal((await response.json()).error.code, "NOT_IMPLEMENTED");
            }

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
