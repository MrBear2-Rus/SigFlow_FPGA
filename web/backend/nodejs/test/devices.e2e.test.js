// 设备枚举：devices list 透传

import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import { rm } from "node:fs/promises";
import test from "node:test";

import { createApp } from "../src/http/server.js";
import { resolveToolPath } from "../src/util/config.js";
import { authedFetch, makeEnvironment, registerAndLogin } from "./helpers.js";

const TOOL = resolveToolPath({});
const skip = existsSync(TOOL) ? false : `sigflow-tool 未构建：${TOOL}`;

test("设备枚举：字段与鉴权", { skip }, async () => {
    const env = await makeEnvironment();
    const app = createApp({
        toolPath: TOOL,
        databaseDir: env.databaseDir,
        userprojectsRoot: env.userprojectsRoot,
    });
    try {
        const address = await app.listen({ host: "127.0.0.1", port: 0 });
        const base = `http://127.0.0.1:${address.port}`;
        assert.equal((await fetch(`${base}/api/devices`)).status, 401);

        const alice = authedFetch(base, await registerAndLogin(base, "alice"));
        const response = await alice("/api/devices");
        assert.equal(response.status, 200);
        const data = await response.json();
        assert.equal(data.schema, "sigflow.devices.list.v1");
        assert.equal(typeof data.platform, "string");
        assert.ok(data.capabilities && typeof data.capabilities.serial === "boolean");
        assert.ok(Array.isArray(data.serial_ports));
        assert.ok(data.ftdi_probe && "device_present" in data.ftdi_probe);
    } finally {
        await app.close();
        await rm(env.root, { recursive: true, force: true });
    }
});
