// 通用一次性 CLI 调用：只解析 JSONL 事件，不解释自由文本。
// 与 analysis-runner 的差别：参数完全由调用方给，不做 --file 约定。

import { spawn } from "node:child_process";

import { JsonlError, JsonlParser } from "../util/jsonl.js";

const MAX_OUTPUT_BYTES = 4 * 1024 * 1024;
const KILL_GRACE_MS = 2000;

/**
 * @param {{ toolPath: string, args: string[], timeoutMs?: number }} options
 * @returns {Promise<{exitCode: number|null, signal: string|null, events: object[], stderr: string, timedOut: boolean, error: string|null}>}
 */
export function runToolCommand({ toolPath, args, timeoutMs = 10_000 }) {
    return new Promise((resolve) => {
        const child = spawn(toolPath, args, { detached: true, stdio: ["ignore", "pipe", "pipe"] });
        const parser = new JsonlParser();
        const events = [];
        let stderr = "";
        let outputBytes = 0;
        let timedOut = false;
        let parseError = null;
        let settled = false;

        const timer = setTimeout(() => {
            timedOut = true;
            try { process.kill(-child.pid, "SIGTERM"); } catch { /* 已退出 */ }
            setTimeout(() => {
                try { process.kill(-child.pid, "SIGKILL"); } catch { /* 已退出 */ }
            }, KILL_GRACE_MS).unref?.();
        }, timeoutMs);
        timer.unref?.();

        child.on("error", (error) => {
            if (settled) return;
            settled = true;
            clearTimeout(timer);
            resolve({ exitCode: null, signal: null, events, stderr, timedOut, error: error.message });
        });

        child.stdout.on("data", (chunk) => {
            outputBytes += chunk.length;
            if (outputBytes > MAX_OUTPUT_BYTES) {
                parseError = new JsonlError("CLI 输出超过上限");
                try { process.kill(-child.pid, "SIGKILL"); } catch { /* 已退出 */ }
                return;
            }
            try {
                events.push(...parser.feed(chunk));
            } catch (error) {
                parseError = error;
                try { process.kill(-child.pid, "SIGKILL"); } catch { /* 已退出 */ }
            }
        });

        child.stderr.on("data", (chunk) => {
            if (stderr.length < 64 * 1024) stderr += chunk.toString("utf8");
        });

        child.on("close", (code, signal) => {
            if (settled) return;
            settled = true;
            clearTimeout(timer);
            try {
                parser.finish();
            } catch (error) {
                parseError ??= error;
            }
            resolve({
                exitCode: code,
                signal,
                events,
                stderr,
                timedOut,
                error: parseError ? parseError.message : null,
            });
        });
    });
}
