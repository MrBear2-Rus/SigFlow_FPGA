// =============================================================================
// 调用一次 sigflow-tool run
//
// 严格遵守 C++ 侧契约（contracts/run-v1 + handoff-cpp.md）：
//   * detached spawn，取消时 kill(-pid, SIGTERM)，宽限期后 SIGKILL；
//   * 参数走 --params -，写完立即关闭 stdin；
//   * stdout 只按有界 JSONL 解析；结尾残行视为协议错误；
//   * 退出后读取 reports/report.json，state 以报告为准；
//   * CLI 退出码 0/2/3/4 与 report.exit_code（工具退出码）分开处理。
// =============================================================================

import { spawn } from "node:child_process";
import { readFile, stat } from "node:fs/promises";

import { JsonlError, JsonlParser } from "../util/jsonl.js";
import { reportPath } from "./job-dir.js";

const MAX_REPORT_BYTES = 2 * 1024 * 1024;
const MAX_STDERR_BYTES = 64 * 1024;
/** 超时看门狗在 C++ 自身超时之后再等这么久，防止 CLI 卡死。 */
const WATCHDOG_EXTRA_MS = 30_000;

/**
 * @param {{
 *   toolPath: string,
 *   projectDir: string,
 *   projectId: string,
 *   jobType: string,
 *   jobId: string,
 *   jobDir: string,
 *   params: object,
 *   timeoutSec?: number,
 *   maxOutputBytes?: number,
 *   cancelGraceMs?: number,
 *   toolRoot?: string,
 *   dataDir?: string,
 *   toolEnv?: Record<string, string>,
 *   signal?: AbortSignal,
 *   onEvent?: (event: any) => void,
 *   onSpawn?: (pid: number) => void,
 * }} options
 */
export async function runToolJob(options) {
    const {
        toolPath,
        projectDir,
        projectId,
        jobType,
        jobId,
        jobDir,
        params,
        timeoutSec = 0,
        maxOutputBytes = 0,
        cancelGraceMs = 5000,
        toolRoot = "",
        dataDir = "",
        toolEnv = {},
        signal,
        onEvent = () => {},
        onSpawn = () => {},
    } = options;

    const args = [
        "run",
        "--job-type", jobType,
        "--project", projectDir,
        "--project-id", projectId,
        "--job-id", jobId,
        "--work-dir", jobDir,
        "--params", "-",
    ];
    if (timeoutSec > 0) args.push("--timeout-sec", String(timeoutSec));
    if (maxOutputBytes > 0) args.push("--max-output-bytes", String(maxOutputBytes));
    if (toolRoot) args.push("--tool-root", toolRoot);
    if (dataDir) args.push("--data-dir", dataDir);

    const child = spawn(toolPath, args, {
        detached: true,
        stdio: ["pipe", "pipe", "pipe"],
        env: Object.keys(toolEnv).length > 0 ? { ...process.env, ...toolEnv } : process.env,
    });

    // 立即上报 pid：取消与进程存活检查都依赖它，不能等任务结束才拿得到。
    if (child.pid) onSpawn(child.pid);

    const parser = new JsonlParser();
    const events = [];
    let stderr = "";
    let cancelReason = null;
    let killTimer = null;
    let watchdog = null;
    let outputError = null;
    let settled = false;

    const cleanup = () => {
        if (killTimer) clearTimeout(killTimer);
        if (watchdog) clearTimeout(watchdog);
        signal?.removeEventListener?.("abort", onAbort);
    };

    const requestCancel = (reason) => {
        if (settled || cancelReason) return;
        cancelReason = reason;
        try {
            process.kill(-child.pid, "SIGTERM");
        } catch {
            try { child.kill("SIGTERM"); } catch { /* 已退出 */ }
        }
        killTimer = setTimeout(() => {
            try { process.kill(-child.pid, "SIGKILL"); } catch { /* 已退出 */ }
        }, cancelGraceMs);
        killTimer.unref?.();
    };

    const onAbort = () => requestCancel("aborted");
    if (signal) {
        if (signal.aborted) onAbort();
        else signal.addEventListener("abort", onAbort, { once: true });
    }

    return new Promise((resolve) => {
        child.on("error", (error) => {
            if (settled) return;
            settled = true;
            cleanup();
            resolve({
                jobId,
                pid: child.pid ?? null,
                state: "Failed",
                exitCode: null,
                signal: null,
                report: null,
                events,
                stderr,
                error: `无法启动 sigflow-tool：${error.message}`,
                cancelReason,
            });
        });

        child.stdout.on("data", (chunk) => {
            try {
                for (const event of parser.feed(chunk)) {
                    events.push(event);
                    try { onEvent(event); } catch { /* 订阅者异常不影响任务 */ }
                }
            } catch (error) {
                outputError = error;
                requestCancel("protocol-error");
            }
        });

        child.stderr.on("data", (chunk) => {
            if (stderr.length >= MAX_STDERR_BYTES) return;
            const text = chunk.toString("utf8");
            stderr += text.slice(0, MAX_STDERR_BYTES - stderr.length);
        });

        // 子进程可能先于 stdin 写完就退出：忽略 EPIPE，由 close 事件统一判定。
        child.stdin.on("error", () => {});
        child.stdin.end(JSON.stringify(params));

        if (timeoutSec > 0) {
            watchdog = setTimeout(
                () => requestCancel("timeout-watchdog"),
                (timeoutSec + WATCHDOG_EXTRA_MS / 1000) * 1000,
            );
            watchdog.unref?.();
        }

        child.on("close", async (code, exitSignal) => {
            if (settled) return;
            settled = true;
            cleanup();

            let trailingLine = null;
            try {
                trailingLine = parser.finish();
            } catch (error) {
                outputError ??= error;
            }
            if (trailingLine && !outputError) {
                outputError = new JsonlError("stdout 结尾存在不完整 JSON 行");
            }

            const report = await readReport(jobDir);
            const state = deriveState({ report, code, exitSignal, cancelReason, outputError });

            resolve({
                jobId,
                pid: child.pid ?? null,
                state,
                exitCode: code,
                signal: exitSignal,
                report,
                events,
                stderr,
                error: outputError ? outputError.message : null,
                cancelReason,
                trailingLine,
            });
        });
    });
}

async function readReport(jobDir) {
    const path = reportPath(jobDir);
    try {
        const info = await stat(path);
        if (info.size > MAX_REPORT_BYTES) return null;
        return JSON.parse(await readFile(path, "utf8"));
    } catch {
        return null;
    }
}

function deriveState({ report, code, exitSignal, cancelReason, outputError }) {
    if (report && typeof report.state === "string" && report.state !== "") return report.state;
    if (cancelReason === "timeout-watchdog") return "TimedOut";
    if (cancelReason) return "Cancelled";
    if (outputError) return "Failed";
    if (code === 3) return "NotImplemented";
    if (code === 0) return "Failed"; // 成功路径必须有报告；缺失按失败处理
    if (exitSignal) return "Failed";
    return "Failed";
}
