// =============================================================================
// 任务链路集成测试：登录 → 建工程 → 提交 synth → SSE → 产物
// =============================================================================

import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import { readFile, readdir, rm } from "node:fs/promises";
import { join } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

import { createApp } from "../src/http/server.js";
import { resolveToolPath } from "../src/util/config.js";
import { authedFetch, makeEnvironment, registerAndLogin, synthParams, waitFor } from "./helpers.js";

const TOOL = resolveToolPath({});
const skip = existsSync(TOOL) ? false : `sigflow-tool 未构建：${TOOL}`;

async function startApp({ sleepSeconds = 0 } = {}) {
    const env = await makeEnvironment({ sleepSeconds });
    const app = createApp({
        toolPath: TOOL,
        databaseDir: env.databaseDir,
        userprojectsRoot: env.userprojectsRoot,
        toolRoot: env.toolsDir,
    });
    const address = await app.listen({ host: "127.0.0.1", port: 0 });
    return { env, app, base: `http://127.0.0.1:${address.port}` };
}

const SOURCE = "module top(input a, output b);\n  assign b = a;\nendmodule\n";

async function prepareProject(authed) {
    await authed("/api/projects", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ projectId: "demo", name: "Demo" }),
    });
    await authed("/api/projects/demo/file?path=src/top.v", {
        method: "PUT",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ content: SOURCE }),
    });
}

test("任务链路：提交、SSE、报告、产物、静态文件", { skip }, async () => {
    const { env, app, base } = await startApp();
    try {
        assert.equal((await fetch(`${base}/api/jobs`)).status, 401);

        const cookie = await registerAndLogin(base);
        const authed = authedFetch(base, cookie);
        await prepareProject(authed);

        let response = await authed("/api/jobs", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ projectId: "demo", jobType: "synth", params: synthParams() }),
        });
        assert.equal(response.status, 202);
        const created = await response.json();

        const sse = await fetch(`${base}/api/jobs/${created.id}/events`, {
            headers: { cookie },
        });
        assert.equal(sse.status, 200);
        const text = await sse.text();
        assert.match(text, /event: end/);
        assert.match(text, /"type":"result"/);
        assert.match(text, /"type":"progress"/);

        response = await authed(`/api/jobs/${created.id}`);
        const detail = await response.json();
        assert.equal(detail.state, "Succeeded", detail.error ?? "");
        assert.equal(detail.artifacts.length, 1);
        assert.equal(detail.artifacts[0].path, "artifacts/top.json");

        response = await authed(`/api/jobs/${created.id}/artifacts/top.json`);
        assert.equal(response.status, 200);
        assert.ok((await response.json()).modules.top);

        response = await authed(`/api/jobs/${created.id}/report`);
        assert.equal((await response.json()).job_id, created.id);

        // 任务隔离：另一个用户看不到
        const bob = authedFetch(base, await registerAndLogin(base, "bob"));
        assert.equal((await bob(`/api/jobs/${created.id}`)).status, 404);

        // 错误参数
        assert.equal(
            (await authed("/api/jobs", { method: "POST", body: "{}" })).status,
            400,
        );
        assert.equal(
            (
                await authed("/api/jobs", {
                    method: "POST",
                    body: JSON.stringify({ projectId: "nope", jobType: "synth", params: {} }),
                })
            ).status,
            404,
        );

        // 静态文件
        const index = await fetch(`${base}/`);
        assert.equal(index.status, 200);
        assert.match(index.headers.get("content-type") ?? "", /text\/html/);
        const home = await fetch(`${base}/`);
        assert.equal(home.status, 200);
        const homeHtml = await home.text();
        assert.match(homeHtml, /SigFlow/);
        assert.match(homeHtml, /登录/);

        for (const asset of [
            "/pages/projects.html",
            "/styles/projects.css",
            "/scripts/projects.js",
            "/pages/workspace.html",
            "/styles/workspace.css",
            "/scripts/workspace.js",
            "/scripts/workspace/canvas-editor.js",
            "/scripts/workspace/editor.js",
            "/scripts/workspace/file-tree.js",
            "/scripts/workspace/analysis.js",
            "/assets/component-icons.json",
            "/pages/fpga-tool.html",
            "/pages/tracebridge.html",
            "/pages/debug-contract.html",
            "/styles/window.css",
            "/scripts/fpga-tool.js",
            "/scripts/window-common.js",
            "/scripts/tracebridge.js",
            "/scripts/debug-contract.js",
            "/styles/landing.css",
            "/scripts/landing.js",
            "/vendor/codemirror/lib/codemirror.js",
            "/vendor/codemirror/mode/verilog/verilog.js",
            "/vendor/codemirror/lib/codemirror.css",
            "/styles/theme.css",
            "/styles/components.css",
            "/scripts/lib/tabs.js",
            "/scripts/lib/splitter.js",
            "/scripts/lib/dialog.js",
            "/scripts/lib/toast.js",
            "/scripts/lib/api.js",
            "/scripts/lib/icons.js",
            "/pages/components.html",
            "/assets/icons/ui/folder.svg",
            "/assets/icons/manifest.json",
        ]) {
            assert.equal((await fetch(`${base}${asset}`)).status, 200, asset);
        }

        // 所有页面里的静态引用都必须可访问（防止删文件后留下断链）
        const pagesDir = fileURLToPath(new URL("../../../frontend/pages/", import.meta.url));
        for (const file of await readdir(pagesDir)) {
            if (!file.endsWith(".html")) continue;
            const html = await readFile(join(pagesDir, file), "utf8");
            const refs = new Set(
                [...html.matchAll(/(?:src|href)="(\/[^"]*)"/g)].map((match) => match[1]),
            );
            for (const ref of refs) {
                const response = await fetch(`${base}${ref}`);
                assert.equal(response.status, 200, `${file} -> ${ref}`);
            }
        }
    } finally {
        await app.close();
        await rm(env.root, { recursive: true, force: true });
    }
});

test("任务取消：运行中取消并落到 Cancelled", { skip }, async () => {
    const { env, app, base } = await startApp({ sleepSeconds: 30 });
    try {
        const authed = authedFetch(base, await registerAndLogin(base));
        await prepareProject(authed);

        const created = await (
            await authed("/api/jobs", {
                method: "POST",
                headers: { "content-type": "application/json" },
                body: JSON.stringify({ projectId: "demo", jobType: "synth", params: synthParams() }),
            })
        ).json();

        await waitFor(async () => {
            const detail = await (await authed(`/api/jobs/${created.id}`)).json();
            return detail.state === "Running";
        });

        const canceled = await authed(`/api/jobs/${created.id}/cancel`, { method: "POST" });
        assert.equal(canceled.status, 200);

        await waitFor(async () => {
            const detail = await (await authed(`/api/jobs/${created.id}`)).json();
            return detail.state === "Cancelled";
        }, 10_000);
    } finally {
        await app.close();
        await rm(env.root, { recursive: true, force: true });
    }
});
