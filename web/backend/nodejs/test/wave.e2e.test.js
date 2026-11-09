// 波形接口：wave/open -> signals -> query -> close（真实 sigflow-tool wave serve）

import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import { mkdir, rm, writeFile } from "node:fs/promises";
import { join } from "node:path";
import test from "node:test";

import { createApp } from "../src/http/server.js";
import { resolveToolPath } from "../src/util/config.js";
import { authedFetch, makeEnvironment, registerAndLogin } from "./helpers.js";

const TOOL = resolveToolPath({});
const skip = existsSync(TOOL) ? false : `sigflow-tool 未构建：${TOOL}`;

const VCD = `$timescale 1ns $end
$scope module top $end
$var wire 1 ! clk $end
$var reg 4 " count [3:0] $end
$upscope $end
$enddefinitions $end
#0
0!
b0000 "
#5
1!
#10
0!
b0001 "
#15
1!
#20
0!
b0010 "
`;

test("波形：open、signals、query、close", { skip }, async () => {
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

        const relative = ".sigflow/agent/jobs/sim.run/job-1/artifacts/waveform/wave.vcd";
        const absolute = join(env.userprojectsRoot, "alice", "demo", relative);
        await mkdir(join(absolute, ".."), { recursive: true });
        await writeFile(absolute, VCD);

        let response = await alice("/api/projects/demo/wave/open", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ vcd: relative }),
        });
        assert.equal(response.status, 200);
        const opened = await response.json();
        assert.equal(opened.signal_count, 2);
        assert.equal(opened.time_range.end, "20");

        response = await alice("/api/projects/demo/wave/signals?offset=0&limit=10");
        assert.equal(response.status, 200);
        const signals = await response.json();
        assert.equal(signals.signals.length, 2);
        assert.equal(signals.signals[1].full_name, "top.count");

        response = await alice("/api/projects/demo/wave/query", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ signal_id: 1, begin: "0", end: "20", limit: 10 }),
        });
        assert.equal(response.status, 200);
        const query = await response.json();
        assert.deepEqual(query.transitions.map((item) => item.time), ["0", "10", "20"]);
        assert.deepEqual(query.transitions.map((item) => item.value), ["0000", "0001", "0010"]);

        response = await alice("/api/projects/demo/wave/open", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ vcd: "src/top.v" }),
        });
        assert.equal(response.status, 400);

        response = await alice("/api/projects/demo/wave/close", { method: "POST" });
        assert.equal(response.status, 200);

        response = await alice("/api/projects/demo/wave/signals");
        assert.equal(response.status, 400);
    } finally {
        await app.close();
        await rm(env.root, { recursive: true, force: true });
    }
});
