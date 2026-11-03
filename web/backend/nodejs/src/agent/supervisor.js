// =============================================================================
// Python Agent 托管：spawn sidecar、下发 bootstrap、等待 ready、健康轮询、退避重启
//
// 协议（与 sigflow_edu_agent 对齐）：
//   stdin 一行 {"type":"sigflow-bootstrap","protocol":"edu.api.v1", instance_id,
//               nonce, gateway_url, gateway_token, ui_token, data_directory}
//   stdout 一行 {"type":"ready","protocol":"edu.api.v1","nonce":…,"port":…,"agent_version":…}
//   stdout 在 ready 之前出现任何字节都视为协议违规。
//   UI 侧鉴权：Authorization: Bearer <ui_token>。
// =============================================================================

import { spawn } from "node:child_process";
import { randomBytes } from "node:crypto";

const PROTOCOL = "edu.api.v1";
const BOOTSTRAP_TYPE = "sigflow-bootstrap";
const READY_TYPE = "ready";
const DEFAULT_READY_TIMEOUT_MS = 10_000;
const MAX_BACKOFF_MS = 30_000;

export class AgentSupervisor {
    constructor({
        python = "python3",
        module = "sigflow_edu_agent",
        script = "",
        dataDir,
        gatewayUrl,
        gatewayToken,
        uiToken,
        logger,
        backoffSec = 1,
        healthPollSec = 5,
        readyTimeoutMs = DEFAULT_READY_TIMEOUT_MS,
    }) {
        this.python = python;
        this.module = module;
        this.script = script;
        this.dataDir = dataDir;
        this.gatewayUrl = gatewayUrl;
        this.gatewayToken = gatewayToken;
        this.uiToken = uiToken;
        this.logger = logger;
        this.backoffSec = Math.max(1, backoffSec);
        this.healthPollSec = Math.max(1, healthPollSec);
        this.readyTimeoutMs = readyTimeoutMs;

        this.child = null;
        this.port = 0;
        this.ready = false;
        this.instanceId = `web-${randomBytes(8).toString("hex")}`;
        this.restartAttempts = 0;
        this.timer = null;
        this.stopping = false;
        this.healthTimer = null;
    }

    get baseUrl() {
        return this.port ? `http://127.0.0.1:${this.port}` : "";
    }

    /** 启动并等待 ready；失败时抛错，调用方可决定是否重试。 */
    start() {
        return new Promise((resolve, reject) => {
            const nonce = randomBytes(16).toString("hex");
            const baseArgs = this.script ? [this.script] : ["-m", this.module];
            const child = spawn(this.python, [...baseArgs, "--bootstrap-stdin", "--port", "0"], {
                stdio: ["pipe", "pipe", "pipe"],
                detached: false,
            });
            this.child = child;
            this.ready = false;
            this.port = 0;

            let settled = false;
            let buffer = "";
            const finish = (error) => {
                if (settled) return;
                settled = true;
                clearTimeout(timer);
                error ? reject(error) : resolve();
            };
            const timer = setTimeout(() => {
                finish(new Error(`agent ready 超时（${this.readyTimeoutMs}ms）`));
                this.#kill("SIGKILL");
            }, this.readyTimeoutMs);

            child.stdout.setEncoding("utf8");
            child.stdout.on("data", (chunk) => {
                buffer += chunk;
                let index;
                while ((index = buffer.indexOf("\n")) >= 0) {
                    const line = buffer.slice(0, index).trim();
                    buffer = buffer.slice(index + 1);
                    if (!line) continue;
                    let record;
                    try {
                        record = JSON.parse(line);
                    } catch {
                        this.logger?.warn?.(`agent stdout 非 JSON：${line.slice(0, 120)}`);
                        continue;
                    }
                    if (record.type === READY_TYPE) {
                        if (record.protocol !== PROTOCOL || record.nonce !== nonce) {
                            finish(new Error("agent ready 记录协议或 nonce 不匹配"));
                            this.#kill("SIGKILL");
                            return;
                        }
                        this.port = Number(record.port) || 0;
                        this.ready = this.port > 0;
                        this.restartAttempts = 0;
                        this.logger?.info?.(`agent ready：${this.baseUrl} (v${record.agent_version ?? "?"})`);
                        this.#startHealth();
                        finish(null);
                        return;
                    }
                }
            });

            child.stderr.setEncoding("utf8");
            child.stderr.on("data", (chunk) => {
                const text = chunk.trim();
                if (text) this.logger?.warn?.(`agent stderr：${text.slice(0, 400)}`);
            });

            child.on("error", (error) => finish(new Error(`agent 启动失败：${error.message}`)));

            child.on("exit", (code, signal) => {
                this.ready = false;
                this.#stopHealth();
                if (this.stopping) return;
                this.logger?.warn?.(`agent 退出 code=${code} signal=${signal}`);
                if (!settled) finish(new Error(`agent 在 ready 之前退出（code=${code}）`));
                this.#scheduleRestart();
            });

            const bootstrap = {
                type: BOOTSTRAP_TYPE,
                protocol: PROTOCOL,
                instance_id: this.instanceId,
                nonce,
                gateway_url: this.gatewayUrl,
                gateway_token: this.gatewayToken,
                ui_token: this.uiToken,
                data_directory: this.dataDir,
            };
            child.stdin.write(`${JSON.stringify(bootstrap)}\n`);
            child.stdin.end();
        });
    }

    /** UI 侧健康检查：GET /api/v1/health（Bearer ui_token）。 */
    async health() {
        if (!this.ready) return { ok: false, reason: "not-ready" };
        try {
            const response = await fetch(`${this.baseUrl}/api/v1/health`, {
                headers: { authorization: `Bearer ${this.uiToken}` },
                signal: AbortSignal.timeout(3000),
            });
            return { ok: response.ok, status: response.status };
        } catch (error) {
            return { ok: false, reason: error.message };
        }
    }

    async stop() {
        this.stopping = true;
        clearTimeout(this.timer);
        this.#stopHealth();
        const child = this.child;
        if (!child) return;
        this.child = null;
        child.kill("SIGTERM");
        await new Promise((resolve) => {
            const timer = setTimeout(() => {
                child.kill("SIGKILL");
                resolve();
            }, 3000);
            child.once("exit", () => {
                clearTimeout(timer);
                resolve();
            });
        });
    }

    // ---- 内部 ---------------------------------------------------------------
    #scheduleRestart() {
        if (this.stopping) return;
        const attempt = this.restartAttempts++;
        const delay = Math.min(MAX_BACKOFF_MS, this.backoffSec * 1000 * 2 ** attempt);
        this.logger?.warn?.(`agent ${delay}ms 后重启（第 ${attempt + 1} 次）`);
        clearTimeout(this.timer);
        this.timer = setTimeout(() => {
            if (this.stopping) return;
            this.start().catch((error) => this.logger?.warn?.(`agent 重启失败：${error.message}`));
        }, delay);
        this.timer.unref?.();
    }

    #startHealth() {
        this.#stopHealth();
        this.healthTimer = setInterval(async () => {
            const result = await this.health();
            if (!result.ok) this.logger?.warn?.(`agent 健康检查失败：${result.status ?? result.reason}`);
        }, this.healthPollSec * 1000);
        this.healthTimer.unref?.();
    }

    #stopHealth() {
        if (this.healthTimer) clearInterval(this.healthTimer);
        this.healthTimer = null;
    }

    #kill(signal) {
        try {
            this.child?.kill(signal);
        } catch {
            /* 已退出 */
        }
    }
}
