// =============================================================================
// J1 联调测试：Node ↔ sigflow-tool
//
// 需要先构建 C++ 侧：
//   cmake -S web -B build/web && cmake --build build/web -j
// 可通过 SIGFLOW_TOOL_BIN 指定其它路径；找不到时相关用例跳过。
// =============================================================================

import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import { access, chmod, mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

import { JobManager } from "../src/jobs/job-manager.js";
import { prepareJobDir } from "../src/jobs/job-dir.js";
import { JsonlParser } from "../src/util/jsonl.js";
import { WEB_ROOT, resolveToolPath } from "../src/util/config.js";

const TOOL = resolveToolPath({});
const TOOL_AVAILABLE = existsSync(TOOL);
const skipReason = TOOL_AVAILABLE ? false : `sigflow-tool 未构建：${TOOL}`;

/** 生成一个假 yosys：读取 write_json 路径并写入最小合法网表。 */
async function makeFakeYosys(dir, { sleepSeconds = 0 } = {}) {
    const path = join(dir, "yosys");
    const script = `#!/bin/sh
sleep ${sleepSeconds}
script=""
while [ $# -gt 0 ]; do
  case "$1" in
    -s) script="$2"; shift 2 ;;
    *) shift ;;
  esac
done
[ -n "$script" ] || exit 9
out=$(sed -n 's/^write_json "\\(.*\\)"$/\\1/p' "$script" | tail -n 1)
[ -n "$out" ] || exit 9
printf '{"modules":{"top":{"ports":{},"cells":{},"netnames":{}}}}' > "$out"
`;
    await writeFile(path, script);
    await chmod(path, 0o755);
    return path;
}

async function makeProject({ sleepSeconds = 0 } = {}) {
    const root = await mkdtemp(join(tmpdir(), "sigflow-js-"));
    const projectDir = join(root, "project-1");
    await mkdir(join(projectDir, "src"), { recursive: true });
    await writeFile(
        join(projectDir, "src", "top.v"),
        "module top(input a, output b);\n  assign b = a;\nendmodule\n",
    );
    const toolsDir = join(root, "tools");
    await mkdir(toolsDir, { recursive: true });
    await makeFakeYosys(toolsDir, { sleepSeconds });
    return { root, projectDir, toolsDir };
}

function synthParams(projectDir) {
    return {
        top_module: "top",
        source_files: [join(projectDir, "src", "top.v")],
        strategy: "baseline",
        yosys_family: "gw1n",
    };
}

test("JSONL：跨块拼接、跳过空行、残留行单独返回", () => {
    const parser = new JsonlParser();
    const first = parser.feed(Buffer.from('{"type":"a"}\n{"type":'));
    assert.deepEqual(first, [{ type: "a" }]);
    const second = parser.feed(Buffer.from('"b"}\n\n'));
    assert.deepEqual(second, [{ type: "b" }]);
    assert.equal(parser.finish(), null);

    const trailing = new JsonlParser();
    trailing.feed(Buffer.from('{"type":"c"}'));
    assert.equal(trailing.finish(), '{"type":"c"}');
});

test("Job 目录：非空目录与越界 ID 必须拒绝", async () => {
    const { root, projectDir } = await makeProject();
    try {
        const jobDir = await prepareJobDir(projectDir, "synth", "job-a");
        await writeFile(join(jobDir, "leftover.txt"), "x");
        await assert.rejects(() => prepareJobDir(projectDir, "synth", "job-a"), /非空/);
        await assert.rejects(() => prepareJobDir(projectDir, "synth", ".."), /不合法/);
    } finally {
        await rm(root, { recursive: true, force: true });
    }
});

test("synth 成功路径：报告、产物、事件", { skip: skipReason }, async () => {
    const { root, projectDir, toolsDir } = await makeProject();
    const manager = new JobManager({ toolPath: TOOL, toolRoot: toolsDir, maxGlobal: 2 });
    try {
        const job = manager.submit({
            projectDir,
            projectId: "project-1",
            jobType: "synth",
            params: synthParams(projectDir),
        });
        await job.done;

        assert.equal(job.state, "Succeeded", `error=${job.error ?? ""}`);
        assert.equal(job.exitCode, 0);
        assert.ok(job.jobDir, "应记录 Job 目录");

        const report = JSON.parse(
            await readFile(join(job.jobDir, "reports", "report.json"), "utf8"),
        );
        assert.equal(report.job_id, job.id);
        assert.equal(report.state, "Succeeded");
        assert.equal(report.artifacts.length, 1);

        const artifact = report.artifacts[0];
        assert.equal(artifact.path, "artifacts/top.json");
        assert.match(artifact.sha256, /^[0-9a-f]{64}$/);
        await access(join(job.jobDir, artifact.path));

        const types = job.events.map((event) => event.type);
        assert.ok(types.includes("progress"));
        assert.ok(types.includes("artifact"));
        assert.ok(types.includes("result"));

        await access(join(job.jobDir, "manifest.json"));
        await access(join(job.jobDir, "request.json"));
    } finally {
        await manager.waitForIdle();
        await rm(root, { recursive: true, force: true });
    }
});

test("synth 取消路径：SIGTERM 落报告且不残留进程", { skip: skipReason }, async () => {
    const { root, projectDir, toolsDir } = await makeProject({ sleepSeconds: 30 });
    const manager = new JobManager({ toolPath: TOOL, toolRoot: toolsDir, maxGlobal: 1 });
    try {
        const job = manager.submit({
            projectDir,
            projectId: "project-1",
            jobType: "synth",
            params: synthParams(projectDir),
        });
        await waitFor(() => job.state === "Running" && job.pid, 5000);

        manager.cancel(job.id);
        await job.done;

        assert.equal(job.state, "Cancelled", `error=${job.error ?? ""}`);
        assert.ok(job.signal, "应以信号退出");
        assert.equal(processIsAlive(job.pid), false, "CLI 进程应已退出");

        const report = JSON.parse(
            await readFile(join(job.jobDir, "reports", "report.json"), "utf8"),
        );
        assert.equal(report.state, "Cancelled");
    } finally {
        await manager.waitForIdle();
        await rm(root, { recursive: true, force: true });
    }
});

test("并发上限：达到上限的任务留在 Queued", { skip: skipReason }, async () => {
    const { root, projectDir, toolsDir } = await makeProject({ sleepSeconds: 30 });
    const manager = new JobManager({
        toolPath: TOOL,
        toolRoot: toolsDir,
        maxGlobal: 1,
        maxPerUser: 1,
    });
    try {
        const first = manager.submit({
            projectDir,
            projectId: "project-1",
            jobType: "synth",
            params: synthParams(projectDir),
        });
        await waitFor(() => first.state === "Running", 5000);

        const second = manager.submit({
            projectDir,
            projectId: "project-1",
            jobType: "synth",
            params: synthParams(projectDir),
        });
        assert.equal(second.state, "Queued");

        manager.cancel(first.id);
        manager.cancel(second.id);
        await Promise.all([first.done, second.done]);

        assert.equal(first.state, "Cancelled");
        assert.equal(second.state, "Cancelled");
    } finally {
        await manager.waitForIdle();
        await rm(root, { recursive: true, force: true });
    }
});

function processIsAlive(pid) {
    if (!pid) return false;
    try {
        process.kill(pid, 0);
        return true;
    } catch {
        return false;
    }
}

async function waitFor(predicate, timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        if (predicate()) return;
        await new Promise((resolve) => setTimeout(resolve, 50));
    }
    throw new Error("等待超时");
}
