// Agent 托管：bootstrap 下发、ready 握手、健康检查、优雅停止
// 用真实 Python 跑一个最小 sidecar，验证协议而不是 mock 进程。

import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

import { AgentSupervisor } from "../src/agent/supervisor.js";

const python = spawnSync("python3", ["--version"], { encoding: "utf8" });
const skip = python.status === 0 ? false : "python3 不可用";

const FAKE_AGENT = String.raw`
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer

record = json.loads(sys.stdin.readline())
assert record["type"] == "sigflow-bootstrap", record
assert record["protocol"] == "edu.api.v1", record

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/api/v1/health":
            self.send_response(404); self.end_headers(); return
        if self.headers.get("Authorization") != "Bearer " + record["ui_token"]:
            self.send_response(401); self.end_headers(); return
        body = json.dumps({
            "schema_version": "edu.api.v1",
            "request_id": "req-1",
            "trace_id": "trace-1",
            "data": {"status": "ok"},
        }).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass

server = HTTPServer(("127.0.0.1", 0), Handler)
print(json.dumps({
    "type": "ready",
    "protocol": "edu.api.v1",
    "nonce": record["nonce"],
    "port": server.server_address[1],
    "agent_version": "test",
}), flush=True)
server.serve_forever()
`;

test("Agent 托管：bootstrap、ready、健康检查与停止", { skip }, async () => {
    const root = await mkdtemp(join(tmpdir(), "sigflow-agent-"));
    const script = join(root, "fake_agent.py");
    await writeFile(script, FAKE_AGENT);

    const supervisor = new AgentSupervisor({
        python: "python3",
        script,
        dataDir: root,
        gatewayUrl: "http://127.0.0.1:1",
        gatewayToken: "gateway-token",
        uiToken: "ui-token",
        backoffSec: 1,
        healthPollSec: 60,
    });

    try {
        await supervisor.start();
        assert.equal(supervisor.ready, true);
        assert.ok(supervisor.port > 0);

        const health = await supervisor.health();
        assert.equal(health.ok, true);
        assert.equal(health.status, 200);

        await supervisor.stop();
        assert.equal(supervisor.ready, false);
    } finally {
        await supervisor.stop();
        await rm(root, { recursive: true, force: true });
    }
});

test("Agent 托管：ready 之前的协议违规会被拒绝", { skip }, async () => {
    const root = await mkdtemp(join(tmpdir(), "sigflow-agent-bad-"));
    const script = join(root, "bad_agent.py");
    await writeFile(script, "import sys\nsys.stdin.readline()\nprint('not-json', flush=True)\n");
    const supervisor = new AgentSupervisor({
        python: "python3",
        script,
        dataDir: root,
        gatewayUrl: "http://127.0.0.1:1",
        gatewayToken: "g",
        uiToken: "u",
        readyTimeoutMs: 2000,
    });
    try {
        await assert.rejects(() => supervisor.start());
    } finally {
        supervisor.stopping = true;
        await supervisor.stop();
        await rm(root, { recursive: true, force: true });
    }
});
