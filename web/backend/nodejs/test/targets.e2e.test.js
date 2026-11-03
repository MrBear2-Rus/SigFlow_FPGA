// 目标板数据与约束校验：target profiles、constraints 读写、validate cst

import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import { rm } from "node:fs/promises";
import test from "node:test";

import { createApp } from "../src/http/server.js";
import { resolveToolPath } from "../src/util/config.js";
import { authedFetch, makeEnvironment, registerAndLogin } from "./helpers.js";

const TOOL = resolveToolPath({});
const skip = existsSync(TOOL) ? false : `sigflow-tool 未构建：${TOOL}`;

test("目标板与约束：列表、profile、CST 读写与校验", { skip }, async () => {
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

        assert.equal((await fetch(`${base}/api/targets`)).status, 401);

        let response = await alice("/api/targets");
        assert.equal(response.status, 200);
        const list = (await response.json()).targets;
        assert.ok(list.some((target) => target.id === "tang-nano-9k"));

        response = await alice("/api/targets/tang-nano-9k");
        assert.equal(response.status, 200);
        const board = await response.json();
        assert.equal(board.profile.id, "tang-nano-9k");
        assert.ok(Array.isArray(board.pins.pins) && board.pins.pins.length > 0);
        assert.equal((await alice("/api/targets/nope")).status, 404);

        await alice("/api/projects", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ projectId: "demo" }),
        });

        // 空约束
        response = await alice("/api/projects/demo/constraints?target=tang-nano-9k");
        assert.equal((await response.json()).content, "");

        const good = 'IO_LOC "led" 10;\nIO_PORT "led" IO_TYPE=LVCMOS33;\n';
        response = await alice("/api/projects/demo/constraints?target=tang-nano-9k", {
            method: "PUT",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ content: good }),
        });
        assert.equal(response.status, 200);
        response = await alice("/api/projects/demo/constraints?target=tang-nano-9k");
        assert.equal((await response.json()).content, good);

        response = await alice("/api/projects/demo/validate-cst", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ target: "tang-nano-9k" }),
        });
        assert.equal(response.status, 200);
        const valid = await response.json();
        assert.equal(valid.valid, true);
        assert.equal(valid.io_loc_count, 1);

        // 保留引脚 -> valid=false，仍是一次成功校验
        await alice("/api/projects/demo/constraints?target=tang-nano-9k", {
            method: "PUT",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ content: 'IO_LOC "bad" 7;\n' }),
        });
        response = await alice("/api/projects/demo/validate-cst", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ target: "tang-nano-9k" }),
        });
        assert.equal(response.status, 200);
        const invalid = await response.json();
        assert.equal(invalid.valid, false);
        assert.ok(invalid.diagnostics.length > 0);

        assert.equal((await alice("/api/projects/demo/constraints")).status, 400);
    } finally {
        await app.close();
        await rm(env.root, { recursive: true, force: true });
    }
});
