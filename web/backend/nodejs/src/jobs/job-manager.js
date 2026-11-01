// =============================================================================
// Job 调度：队列 + 并发上限 + 取消
//
// 职责边界（README 决策 D-02）：队列、并发、状态合成全在 Node；
// C++ 每次只执行一个 Job，不保存跨调用状态。
// =============================================================================

import { prepareJobDir } from "./job-dir.js";
import { newJobId, isValidId } from "./job-id.js";
import { runToolJob } from "./tool-runner.js";

export class JobManager {
    /**
     * @param {{
     *   toolPath: string,
     *   maxGlobal?: number,
     *   maxPerUser?: number,
     *   defaultTimeoutSec?: number,
     *   maxOutputBytes?: number,
     *   cancelGraceMs?: number,
     *   toolRoot?: string,
     *   dataDir?: string,
     *   toolEnv?: Record<string, string>,
     *   logger?: { info: Function, warn: Function, error: Function, debug: Function },
     * }} options
     */
    constructor({
        toolPath,
        maxGlobal = 4,
        maxPerUser = 2,
        defaultTimeoutSec = 0,
        maxOutputBytes = 0,
        cancelGraceMs = 5000,
        toolRoot = "",
        dataDir = "",
        toolEnv = {},
        logger = null,
    } = {}) {
        if (!toolPath) throw new Error("JobManager 需要 toolPath");
        this.toolPath = toolPath;
        this.maxGlobal = Math.max(1, maxGlobal);
        this.maxPerUser = Math.max(1, maxPerUser);
        this.defaultTimeoutSec = defaultTimeoutSec;
        this.maxOutputBytes = maxOutputBytes;
        this.cancelGraceMs = cancelGraceMs;
        this.toolRoot = toolRoot;
        this.dataDir = dataDir;
        this.toolEnv = toolEnv;
        this.logger = logger;

        /** @type {Map<string, object>} */
        this.jobs = new Map();
        /** @type {object[]} */
        this.queue = [];
        /** @type {Map<string, object>} */
        this.running = new Map();
        /** @type {Array<Function>} */
        this.idleWaiters = [];
    }

    /**
     * 提交一个 Job。立即返回任务句柄，执行由队列驱动。
     * @param {{
     *   ownerId?: string,
     *   projectDir: string,
     *   projectId: string,
     *   jobType: string,
     *   params: object,
     *   timeoutSec?: number,
     *   maxOutputBytes?: number,
     *   onEvent?: (event: any, job: object) => void,
     * }} request
     */
    submit({
        ownerId = "local",
        projectDir,
        projectId,
        jobType,
        params,
        timeoutSec,
        maxOutputBytes,
        onEvent,
    }) {
        if (!isValidId(projectId)) throw new Error(`projectId 不合法：${projectId}`);
        if (!projectDir) throw new Error("缺少 projectDir");
        if (params === null || typeof params !== "object" || Array.isArray(params)) {
            throw new Error("params 必须是 JSON 对象");
        }

        const id = newJobId();
        let resolveDone;
        const done = new Promise((resolve) => { resolveDone = resolve; });

        const job = {
            id,
            ownerId,
            projectId,
            jobType,
            state: "Queued",
            createdAt: new Date().toISOString(),
            finishedAt: null,
            jobDir: null,
            pid: null,
            exitCode: null,
            signal: null,
            report: null,
            error: null,
            cancelReason: null,
            events: [],
            done,
            _resolve: resolveDone,
            _projectDir: projectDir,
            _params: params,
            _timeoutSec: timeoutSec ?? this.defaultTimeoutSec,
            _maxOutputBytes: maxOutputBytes ?? this.maxOutputBytes,
            _onEvent: onEvent,
            _controller: new AbortController(),
        };

        this.jobs.set(id, job);
        this.queue.push(job);
        this.logger?.info?.(`Job 入队 ${id} (${jobType}, owner=${ownerId})`);
        this.#pump();
        return job;
    }

    /** 取消排队中或运行中的 Job。 */
    cancel(id, reason = "user") {
        const job = this.jobs.get(id);
        if (!job) return false;

        if (job.state === "Queued") {
            const index = this.queue.indexOf(job);
            if (index >= 0) this.queue.splice(index, 1);
            job.state = "Cancelled";
            job.cancelReason = reason;
            job.error = "排队中被取消";
            job.finishedAt = new Date().toISOString();
            job._resolve(job);
            this.#pump();
            return true;
        }

        if (this.running.has(id)) {
            job.cancelReason = reason;
            job._controller.abort();
            return true;
        }
        return false;
    }

    get(id) {
        return this.jobs.get(id) ?? null;
    }

    list() {
        return [...this.jobs.values()];
    }

    /** 取消所有未完成的任务并等待退出（进程优雅关闭用）。 */
    async shutdown(reason = "shutdown") {
        for (const job of this.list()) {
            if (job.state === "Queued" || job.state === "Running") this.cancel(job.id, reason);
        }
        await this.waitForIdle();
    }

    /** 等待队列清空且没有运行中的任务（测试与优雅退出用）。 */
    waitForIdle() {
        if (this.queue.length === 0 && this.running.size === 0) return Promise.resolve();
        return new Promise((resolve) => this.idleWaiters.push(resolve));
    }

    #runningFor(ownerId) {
        let count = 0;
        for (const job of this.running.values()) {
            if (job.ownerId === ownerId) count += 1;
        }
        return count;
    }

    #pump() {
        while (this.running.size < this.maxGlobal) {
            const index = this.queue.findIndex(
                (job) => this.#runningFor(job.ownerId) < this.maxPerUser,
            );
            if (index < 0) break;
            const [job] = this.queue.splice(index, 1);
            void this.#start(job);
        }
        if (this.queue.length === 0 && this.running.size === 0) {
            const waiters = this.idleWaiters;
            this.idleWaiters = [];
            for (const resolve of waiters) resolve();
        }
    }

    async #start(job) {
        this.running.set(job.id, job);
        job.state = "Running";

        try {
            job.jobDir = await prepareJobDir(job._projectDir, job.jobType, job.id);

            const result = await runToolJob({
                toolPath: this.toolPath,
                projectDir: job._projectDir,
                projectId: job.projectId,
                jobType: job.jobType,
                jobId: job.id,
                jobDir: job.jobDir,
                params: job._params,
                timeoutSec: job._timeoutSec,
                maxOutputBytes: job._maxOutputBytes,
                cancelGraceMs: this.cancelGraceMs,
                toolRoot: this.toolRoot,
                dataDir: this.dataDir,
                toolEnv: this.toolEnv,
                signal: job._controller.signal,
                onSpawn: (pid) => { job.pid = pid; },
                onEvent: (event) => {
                    job.events.push(event);
                    try { job._onEvent?.(event, job); } catch { /* 订阅者异常忽略 */ }
                },
            });

            job.state = result.state;
            job.pid = result.pid;
            job.exitCode = result.exitCode;
            job.signal = result.signal;
            job.report = result.report;
            job.error = result.error;
            job.cancelReason = result.cancelReason;
        } catch (error) {
            job.state = "Failed";
            job.error = error instanceof Error ? error.message : String(error);
            this.logger?.error?.(`Job ${job.id} 失败：${job.error}`);
        } finally {
            this.running.delete(job.id);
            job.finishedAt = new Date().toISOString();
            job._resolve(job);
            this.logger?.info?.(`Job 结束 ${job.id} -> ${job.state}`);
            this.#pump();
        }
    }
}
