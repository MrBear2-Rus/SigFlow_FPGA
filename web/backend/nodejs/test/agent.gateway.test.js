// Agent Gateway 与 UI 代理：用替身 Agent HTTP 服务验证路由与鉴权

import assert from "node:assert/strict";
import { createServer } from "node:http";
import { existsSync } from "node:fs";
import { rm } from "node:fs/promises";
import test from "node:test";

import { createApp } from "../src/http/server.js";
import { resolveToolPath } from "../src/util/config.js";
import { authedFetch, makeEnvironment, registerAndLogin } from "./helpers.js";

const TOOL = resolveToolPath({});
const skip = existsSync(TOOL) ? false : `sigflow-tool 未构建：${TOOL}`;

const envelope = (data) =>
    JSON.stringify({ schema_version: "edu.api.v1", request_id: "r", trace_id: "t", data });

test("Agent 网关与代理：token 鉴权、context/report、转发 ui_token", { skip }, async () => {
    const seen = { authorization: "", body: "" };
    const upstream = createServer((req, res) => {
        seen.authorization = req.headers.authorization ?? "";
        let raw = "";
        req.on("data", (chunk) => (raw += chunk));
        req.on("end", () => {
            seen.body = raw;
            res.setHeader("content-type", "application/json");
            if (req.url === "/api/v1/health") {
                res.end(envelope({ status: "ok" }));
            } else if (req.method === "POST" && req.url === "/api/v1/sessions") {
                res.statusCode = 201;
                res.end(envelope({ session_id: "s1" }));
            } else {
                res.statusCode = 404;
                res.end(JSON.stringify({ error: { code: "NOT_FOUND", message: "nope" } }));
            }
        });
    });
    await new Promise((resolve) => upstream.listen(0, "127.0.0.1", resolve));
    const upstreamUrl = `http://127.0.0.1:${upstream.address().port}`;

    const fakeManager = {
        forUser: async () => ({ supervisor: { baseUrl: upstreamUrl }, uiToken: "ui-token" }),
        resolveToken: (token) =>
            token === "gateway-token" ? { userId: "alice", entry: {} } : null,
    };

    const env = await makeEnvironment();
    const app = createApp({
        toolPath: TOOL,
        databaseDir: env.databaseDir,
        userprojectsRoot: env.userprojectsRoot,
        agentManager: fakeManager,
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

        // 代理：浏览器 Cookie -> Agent ui_token
        let response = await alice("/api/agent/health");
        assert.equal(response.status, 200);
        assert.equal(seen.authorization, "Bearer ui-token");

        response = await alice("/api/agent/sessions", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ project_id: "demo", revision: "1" }),
        });
        assert.equal(response.status, 201);
        assert.equal((await response.json()).data.session_id, "s1");
        assert.match(seen.body, /"project_id":"demo"/);

        // 未登录访问代理
        assert.equal((await fetch(`${base}/api/agent/health`)).status, 401);

        // 网关：gateway_token
        const gateway = (path, token = "gateway-token") =>
            fetch(`${base}${path}`, { headers: { authorization: `Bearer ${token}` } });

        response = await gateway("/api/v1/health");
        assert.equal(response.status, 200);
        assert.equal((await response.json()).schema_version, "edu.api.v1");

        assert.equal((await gateway("/api/v1/health", "wrong")).status, 401);

        response = await gateway("/api/v1/projects/demo/context");
        assert.equal(response.status, 200);
        const context = (await response.json()).data;
        assert.equal(context.project_id, "demo");
        assert.ok(Array.isArray(context.files));

        assert.equal((await gateway("/api/v1/jobs/missing/report")).status, 404);
    } finally {
        await app.close();
        upstream.close();
        await rm(env.root, { recursive: true, force: true });
    }
});
