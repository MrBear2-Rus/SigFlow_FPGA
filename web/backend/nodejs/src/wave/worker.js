// =============================================================================
// 每用户一个 `sigflow-tool wave serve` 子进程
//
// 协议（wave-cpp-draft.md）：
//   请求 {"id", "op": open|signals|query|close, ...}
//   响应 {"id", "ok":true, "data":...} / {"id", "ok":false, "error":{code,message}}
//   每行请求 ≤64 KiB、响应 ≤900 KiB；JSON 解析失败时 id 为 null。
// =============================================================================

import { spawn } from "node:child_process";
import { randomUUID } from "node:crypto";

const REQUEST_TIMEOUT_MS = 15_000;
const MAX_LINE_BYTES = 1024 * 1024;

export class WaveWorker {
    constructor({ toolPath, logger }) {
        this.toolPath = toolPath;
        this.logger = logger;
        this.child = null;
        this.pending = new Map();
        this.buffer = "";
        this.ready = false;
        this.stopping = false;
    }

    async start() {
        if (this.child && this.ready) return;
        const child = spawn(this.toolPath, ["wave", "serve"], {
            stdio: ["pipe", "pipe", "pipe"],
        });
        this.child = child;
        this.ready = true;
        this.stopping = false;

        child.stdout.setEncoding("utf8");
        child.stdout.on("data", (chunk) => this.#onData(chunk));
        child.stderr.setEncoding("utf8");
        child.stderr.on("data", (chunk) => {
            const text = chunk.trim();
            if (text) this.logger?.warn?.(`wave stderr：${text.slice(0, 400)}`);
        });
        child.on("error", (error) => this.#failAll(`wave worker 启动失败：${error.message}`));
        child.on("exit", (code, signal) => {
            this.ready = false;
            if (!this.stopping) this.logger?.warn?.(`wave worker 退出 code=${code} signal=${signal}`);
            this.#failAll("wave worker 已退出");
        });
    }

    /** 发送一条请求；返回 data（失败抛错）。 */
    async request(op, payload = {}) {
        if (!this.child || !this.ready) await this.start();
        const id = randomUUID();
        const line = JSON.stringify({ id, op, ...payload });
        if (Buffer.byteLength(line) > 64 * 1024) {
            throw Object.assign(new Error("wave 请求超过 64 KiB"), { code: "INVALID_ARGUMENT" });
        }
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this.pending.delete(id);
                reject(Object.assign(new Error("wave 请求超时"), { code: "TIMEOUT" }));
            }, REQUEST_TIMEOUT_MS);
            this.pending.set(id, { resolve, reject, timer });
            this.child.stdin.write(`${line}\n`);
        });
    }

    async stop() {
        this.stopping = true;
        this.ready = false;
        this.#failAll("wave worker 已停止");
        if (!this.child) return;
        const child = this.child;
        this.child = null;
        child.stdin.end();
        child.kill("SIGTERM");
        await new Promise((resolve) => {
            const timer = setTimeout(() => {
                child.kill("SIGKILL");
                resolve();
            }, 2000);
            child.once("exit", () => {
                clearTimeout(timer);
                resolve();
            });
        });
    }

    // ---- 内部 ---------------------------------------------------------------
    #onData(chunk) {
        this.buffer += chunk;
        if (Buffer.byteLength(this.buffer) > MAX_LINE_BYTES) {
            this.logger?.warn?.("wave 响应行超过 1 MiB，重启 worker");
            this.#failAll("wave 响应行超过上限");
            void this.stop();
            return;
        }
        let index;
        while ((index = this.buffer.indexOf("\n")) >= 0) {
            const line = this.buffer.slice(0, index).trim();
            this.buffer = this.buffer.slice(index + 1);
            if (!line) continue;
            let message;
            try {
                message = JSON.parse(line);
            } catch {
                this.logger?.warn?.(`wave 输出非 JSON：${line.slice(0, 120)}`);
                continue;
            }
            if (message.id === null || message.id === undefined) {
                // 协议错误（JSON 解析失败或 id 非法）：无法对应请求，按协议错误处理。
                this.logger?.warn?.(`wave 无 id 响应：${message.error?.message ?? ""}`);
                this.#failAll("wave 返回了无 id 的响应");
                void this.stop();
                return;
            }
            const pending = this.pending.get(message.id);
            if (!pending) continue;
            this.pending.delete(message.id);
            clearTimeout(pending.timer);
            if (message.ok) pending.resolve(message.data);
            else {
                const error = new Error(message.error?.message ?? "wave 请求失败");
                error.code = message.error?.code ?? "WAVE_ERROR";
                pending.reject(error);
            }
        }
    }

    #failAll(reason) {
        for (const [id, pending] of this.pending) {
            clearTimeout(pending.timer);
            pending.reject(Object.assign(new Error(reason), { code: "WAVE_UNAVAILABLE" }));
            this.pending.delete(id);
        }
    }
}
