// =============================================================================
// 一次性分析调用：sigflow-tool structure|lint
//
// 契约（待 C++ P6 实现，见 handoff-js H-JS-004）：
//   * 成功：stdout 输出一行 {"type":"result","ok":true,"data":{...}}，退出码 0；
//   * 失败：stdout 输出 diagnostic 事件，退出码 4；
//   * 未实现：退出码 3（Node 侧映射为 501）。
// =============================================================================

import { spawn } from "node:child_process";

import { JsonlError, JsonlParser } from "../util/jsonl.js";

const MAX_OUTPUT_BYTES = 4 * 1024 * 1024;
const KILL_GRACE_MS = 2000;

/**
 * @param {{
 *   toolPath: string,
 *   command: "structure" | "lint",
 *   filePath: string,
 *   dataDir?: string,
 *   timeoutMs?: number,
 * }} options
 */
export function runAnalysis({ toolPath, command, filePath, dataDir = "", timeoutMs = 10_000 }) {
    const args = [command, "--file", filePath];
    if (dataDir) args.push("--data-dir", dataDir);

    return new Promise((resolve) => {
        const child = spawn(toolPath, args, {
            detached: true,
            stdio: ["ignore", "pipe", "pipe"],
        });

        const parser = new JsonlParser();
        const events = [];
        let stderr = "";
        let outputBytes = 0;
        let timedOut = false;
        let parseError = null;
        let settled = false;

        const killTimer = () => {
            try { process.kill(-child.pid, "SIGKILL"); } catch { /* 已退出 */ }
        };
        const timer = setTimeout(() => {
            timedOut = true;
            try { process.kill(-child.pid, "SIGTERM"); } catch { /* 已退出 */ }
            setTimeout(killTimer, KILL_GRACE_MS).unref?.();
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
                parseError = new JsonlError("分析输出超过上限");
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
