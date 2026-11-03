// =============================================================================
// HTTP / SSE 服务
//
// 路由分组：
//   公开：GET /api/health、POST /api/users、POST /api/session
//   会话：GET/DELETE /api/session
//   工程：/api/projects…
//   任务：/api/jobs…（只能看到自己的任务）
//   静态：/static/*（backend/static）、/*（frontend）
//
// 只依赖 Node 内置模块。
// =============================================================================

import { createReadStream } from "node:fs";
import { stat } from "node:fs/promises";
import { createServer as createHttpServer } from "node:http";
import { extname, join, resolve, sep } from "node:path";

import { createAgentGateway } from "../agent/gateway.js";
import { runToolCommand } from "../analysis/tool-command.js";
import { runAnalysis } from "../analysis/analysis-runner.js";
import { JobManager } from "../jobs/job-manager.js";
import { artifactPath } from "../jobs/job-dir.js";
import { ProjectStore } from "../projects/project-store.js";
import { TargetStore } from "../projects/target-store.js";
import {
    SESSION_COOKIE,
    SessionStore,
    readSessionCookie,
    sessionCookie,
} from "../users/session-store.js";
import { UserStore } from "../users/user-store.js";
import { WEB_ROOT } from "../util/config.js";
import { createLogger } from "../util/log.js";

const TERMINAL_STATES = new Set(["Succeeded", "Failed", "Cancelled", "TimedOut", "NotImplemented"]);
const MAX_BODY_BYTES = 2 * 1024 * 1024;
const SSE_POLL_MS = 50;

const MIME_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".ico": "image/x-icon",
    ".woff2": "font/woff2",
    ".vcd": "text/plain; charset=utf-8",
};

class HttpError extends Error {
    constructor(status, message, code = "BAD_REQUEST") {
        super(message);
        this.status = status;
        this.code = code;
    }
}

/** 创建应用（不监听端口）。 */
export function createApp({
    config = {},
    toolPath,
    databaseDir = join(WEB_ROOT, "backend", "database"),
    userprojectsRoot = join(WEB_ROOT, "backend", "userprojects"),
    frontendRoot = join(WEB_ROOT, "frontend"),
    staticRoot = join(WEB_ROOT, "backend", "static"),
    maxGlobal = 4,
    maxPerUser = 2,
    defaultTimeoutSec = 0,
    maxOutputBytes = 0,
    cancelGraceMs = 5000,
    toolRoot = "",
    dataDir = "",
    toolEnv = {},
    sessionTtlSec = 86400,
    maxFileBytes = 2 * 1024 * 1024,
    agentManager = null,
    logger = createLogger({ scope: "http" }),
} = {}) {
    let agentManagerRef = agentManager;
    const users = new UserStore({ file: join(databaseDir, "users.json") });
    const sessions = new SessionStore({
        ttlSec: sessionTtlSec,
        file: join(databaseDir, "sessions.json"),
    });
    const projects = new ProjectStore({ root: userprojectsRoot, maxFileBytes });
    const targetStore = new TargetStore({ root: join(staticRoot, "target-profiles") });
    const manager = new JobManager({
        toolPath,
        maxGlobal,
        maxPerUser,
        defaultTimeoutSec,
        maxOutputBytes,
        cancelGraceMs,
        toolRoot,
        dataDir,
        toolEnv,
        logger,
    });

    const server = createHttpServer((req, res) => {
        handle(req, res).catch((error) => {
            const status = typeof error.status === "number" ? error.status : 500;
            const code = typeof error.code === "string" ? error.code : "INTERNAL";
            if (status >= 500) logger.error?.(`${req.method} ${req.url} 失败：${error.message}`);
            sendJson(res, status, { error: { code, message: error.message } });
        });
    });

    function authenticatedUser(req) {
        const token = readSessionCookie(req.headers.cookie);
        const userId = sessions.resolve(token);
        return userId ? { userId, token } : null;
    }

    async function handle(req, res) {
        const url = new URL(req.url, `http://${req.headers.host ?? "localhost"}`);
        const path = decodeURIComponent(url.pathname);
        const method = req.method ?? "GET";

        // ---- 公开路由 ------------------------------------------------------
        if (path === "/api/health" && method === "GET") {
            return sendJson(res, 200, {
                status: "ok",
                service: "sigflow-web",
                jobs: { running: manager.running.size, queued: manager.queue.length },
                users: (await users.list()).length,
            });
        }
        if (path === "/api/users" && method === "POST") {
            const body = await readJson(req);
            await users.create(body.userId, body.password);
            return sendJson(res, 201, { userId: body.userId });
        }
        if (path === "/api/session" && method === "POST") {
            const body = await readJson(req);
            if (!(await users.verify(body.userId, body.password))) {
                throw new HttpError(401, "用户名或密码错误", "UNAUTHENTICATED");
            }
            const token = sessions.create(body.userId);
            res.setHeader("Set-Cookie", sessionCookie(token, { maxAgeSec: sessionTtlSec }));
            return sendJson(res, 200, { userId: body.userId });
        }

        // ---- Python Agent 的 Gateway 路由（gateway_token 鉴权） -------------
        if (path.startsWith("/api/v1/") && agentManagerRef) {
            return createAgentGateway({ agentManager: agentManagerRef, projects, jobs: manager })(
                req,
                res,
                url,
            );
        }

        // ---- 浏览器访问 Agent 的代理（需要登录） ----------------------------
        if (path === "/api/agent" || path.startsWith("/api/agent/")) {
            const session = authenticatedUser(req);
            if (!session) throw new HttpError(401, "未登录", "UNAUTHENTICATED");
            return proxyAgent(req, res, url, session.userId);
        }

        // ---- 以下都需要登录 -------------------------------------------------
        if (path.startsWith("/api/")) {
            const session = authenticatedUser(req);
            if (!session) throw new HttpError(401, "未登录", "UNAUTHENTICATED");
            return handleApi(req, res, url, path, method, session);
        }

        // ---- 静态资源 -------------------------------------------------------
        if (method === "GET" || method === "HEAD") {
            if (path.startsWith("/static/")) {
                return serveFile(res, staticRoot, path.slice("/static/".length));
            }
            const relative = path === "/" ? "pages/index.html" : path.slice(1);
            return serveFile(res, frontendRoot, relative);
        }

        throw new HttpError(404, "未找到", "NOT_FOUND");
    }

    async function handleApi(req, res, url, path, method, session) {
        const { userId, token } = session;

        if (path === "/api/targets" && method === "GET") {
            return sendJson(res, 200, { targets: await targetStore.list() });
        }
        const targetMatch = /^\/api\/targets\/([^/]+)$/.exec(path);
        if (targetMatch && method === "GET") {
            return sendJson(res, 200, await targetStore.get(decodeURIComponent(targetMatch[1])));
        }

        if (path === "/api/session") {
            if (method === "GET") return sendJson(res, 200, { userId });
            if (method === "DELETE") {
                sessions.destroy(token);
                res.setHeader(
                    "Set-Cookie",
                    `${SESSION_COOKIE}=; Max-Age=0; HttpOnly; SameSite=Lax; Path=/`,
                );
                return sendNoContent(res);
            }
            throw new HttpError(405, "方法不被允许", "METHOD_NOT_ALLOWED");
        }

        if (path === "/api/projects") {
            if (method === "GET") return sendJson(res, 200, { projects: await projects.list(userId) });
            if (method === "POST") {
                const body = await readJson(req);
                const project = await projects.create(userId, body.projectId, {
                    name: body.name,
                    template: body.template,
                });
                return sendJson(res, 201, project);
            }
            throw new HttpError(405, "方法不被允许", "METHOD_NOT_ALLOWED");
        }

        const projectMatch = /^\/api\/projects\/([^/]+)(?:\/(.*))?$/.exec(path);
        if (projectMatch) {
            const [, projectId, rest = ""] = projectMatch;
            return handleProject(req, res, url, method, userId, projectId, rest);
        }

        if (path === "/api/jobs") {
            if (method === "POST") return createJob(req, res, userId);
            if (method === "GET") {
                return sendJson(res, 200, {
                    jobs: manager.list().filter((job) => job.ownerId === userId).map(summarize),
                });
            }
            throw new HttpError(405, "方法不被允许", "METHOD_NOT_ALLOWED");
        }

        const jobMatch = /^\/api\/jobs\/([^/]+)(?:\/(.*))?$/.exec(path);
        if (jobMatch) {
            const [, jobId, rest = ""] = jobMatch;
            const job = manager.get(jobId);
            if (!job || job.ownerId !== userId) {
                throw new HttpError(404, `任务不存在：${jobId}`, "NOT_FOUND");
            }
            return handleJob(req, res, url, job, rest, method);
        }

        throw new HttpError(404, "未找到", "NOT_FOUND");
    }

    async function handleProject(req, res, url, method, userId, projectId, rest) {
        if (rest === "") {
            if (method === "GET") return sendJson(res, 200, await projects.metadata(userId, projectId));
            if (method === "DELETE") {
                await projects.remove(userId, projectId);
                return sendNoContent(res);
            }
            throw new HttpError(405, "方法不被允许", "METHOD_NOT_ALLOWED");
        }
        if (rest === "canvas") {
            if (method === "GET") return sendJson(res, 200, await projects.readCanvas(userId, projectId));
            if (method === "PUT") {
                const body = await readJson(req);
                return sendJson(res, 200, await projects.writeCanvas(userId, projectId, body));
            }
            throw new HttpError(405, "方法不被允许", "METHOD_NOT_ALLOWED");
        }
        if (rest === "constraints") {
            const target = url.searchParams.get("target") ?? "";
            if (!target) throw new HttpError(400, "缺少 target", "MISSING_TARGET");
            const rel = `constraints/${target}.cst`;
            await targetStore.get(target);
            if (method === "GET") {
                try {
                    const content = await projects.readFile(userId, projectId, rel);
                    return sendJson(res, 200, { target, path: rel, content });
                } catch (error) {
                    if (error.code === "FILE_NOT_FOUND") {
                        return sendJson(res, 200, { target, path: rel, content: "" });
                    }
                    throw error;
                }
            }
            if (method === "PUT") {
                const body = await readJson(req);
                if (typeof body.content !== "string") {
                    throw new HttpError(400, "content 必须是字符串", "INVALID_CONTENT");
                }
                await projects.writeFile(userId, projectId, rel, body.content);
                return sendJson(res, 200, { target, path: rel });
            }
            throw new HttpError(405, "方法不被允许", "METHOD_NOT_ALLOWED");
        }
        if (rest === "validate-cst") {
            if (method !== "POST") throw new HttpError(405, "方法不被允许", "METHOD_NOT_ALLOWED");
            const body = await readJson(req);
            const target = body.target;
            if (typeof target !== "string" || target === "") {
                throw new HttpError(400, "缺少 target", "MISSING_TARGET");
            }
            const cstPath = await projects.absoluteFile(userId, projectId, `constraints/${target}.cst`);
            const pinsPath = targetStore.pinsPath(target);
            const result = await runToolCommand({
                toolPath,
                args: ["validate", "cst", "--cst", cstPath, "--pins", pinsPath],
            });
            if (result.exitCode === 3) {
                throw new HttpError(501, "C++ 侧尚未实现 validate", "NOT_IMPLEMENTED");
            }
            const failure = result.events.find(
                (event) => event.type === "diagnostic" && event.severity === "error",
            );
            const resultEvent = result.events.find((event) => event.type === "result" && event.data);
            if (result.exitCode !== 0 || !resultEvent) {
                throw new HttpError(
                    502,
                    failure?.message ?? result.error ?? "约束校验失败",
                    "VALIDATION_FAILED",
                );
            }
            return sendJson(res, 200, resultEvent.data);
        }
        if (rest === "structure" || rest === "lint") {
            if (method !== "POST") throw new HttpError(405, "方法不被允许", "METHOD_NOT_ALLOWED");
            return analyze(req, res, userId, projectId, rest);
        }
        if (rest === "files" && method === "GET") {
            return sendJson(res, 200, await projects.listFiles(userId, projectId));
        }
        if (rest === "file/move") {
            if (method !== "POST") throw new HttpError(405, "方法不被允许", "METHOD_NOT_ALLOWED");
            const body = await readJson(req);
            const result = await projects.moveFile(userId, projectId, body.from, body.to);
            return sendJson(res, 200, result);
        }
        if (rest === "file") {
            const filePath = url.searchParams.get("path") ?? "";
            if (method === "GET") {
                const content = await projects.readFile(userId, projectId, filePath);
                return sendJson(res, 200, { path: filePath, content });
            }
            if (method === "PUT") {
                const body = await readJson(req);
                const result = await projects.writeFile(userId, projectId, filePath, body.content);
                return sendJson(res, 200, result);
            }
            if (method === "DELETE") {
                await projects.removeFile(userId, projectId, filePath);
                return sendNoContent(res);
            }
            throw new HttpError(405, "方法不被允许", "METHOD_NOT_ALLOWED");
        }
        throw new HttpError(404, "未找到", "NOT_FOUND");
    }

    async function proxyAgent(req, res, url, userId) {
        if (!agentManagerRef) throw new HttpError(503, "Agent 未启用", "AGENT_DISABLED");
        let entry;
        try {
            entry = await agentManagerRef.forUser(userId);
        } catch (error) {
            throw new HttpError(502, `Agent 启动失败：${error.message}`, "AGENT_UNAVAILABLE");
        }

        const suffix = url.pathname.slice("/api/agent".length) || "/";
        const target = `${entry.supervisor.baseUrl}/api/v1${suffix}${url.search}`;
        const headers = { authorization: `Bearer ${entry.uiToken}` };
        let body;
        if (req.method !== "GET" && req.method !== "HEAD") {
            body = await readRawBody(req);
            if (body) headers["content-type"] = "application/json";
        }

        let upstream;
        try {
            upstream = await fetch(target, {
                method: req.method,
                headers,
                body,
                signal: AbortSignal.timeout(30_000),
            });
        } catch (error) {
            throw new HttpError(502, `Agent 请求失败：${error.message}`, "AGENT_UNAVAILABLE");
        }
        const text = await upstream.text();
        res.writeHead(upstream.status, { "content-type": "application/json; charset=utf-8" });
        res.end(text);
    }

    async function analyze(req, res, userId, projectId, command) {
        const body = await readJson(req);
        const filePath = await projects.absoluteFile(userId, projectId, body.path);
        const result = await runAnalysis({
            toolPath,
            command,
            filePath,
            projectRoot: projects.projectDir(userId, projectId),
            dataDir,
        });

        if (result.timedOut) throw new HttpError(504, "分析超时", "ANALYSIS_TIMEOUT");
        if (result.exitCode === 3) {
            throw new HttpError(501, "C++ 侧尚未实现该子命令（P6）", "NOT_IMPLEMENTED");
        }
        const failure = result.events.find(
            (event) => event.type === "diagnostic" && event.severity === "error",
        );
        const resultEvent = result.events.find((event) => event.type === "result" && event.data);
        if (result.exitCode !== 0 || !resultEvent) {
            throw new HttpError(
                502,
                failure?.message ?? result.error ?? "分析失败",
                "ANALYSIS_FAILED",
            );
        }
        return sendJson(res, 200, resultEvent.data);
    }

    async function createJob(req, res, userId) {
        const body = await readJson(req);
        const projectId = body.projectId;
        if (typeof projectId !== "string" || projectId === "") {
            throw new HttpError(400, "缺少 projectId");
        }
        if (typeof body.jobType !== "string") throw new HttpError(400, "缺少 jobType");
        if (body.params === null || typeof body.params !== "object" || Array.isArray(body.params)) {
            throw new HttpError(400, "params 必须是 JSON 对象");
        }
        if (!(await projects.exists(userId, projectId))) {
            throw new HttpError(404, `工程不存在：${projectId}`, "PROJECT_NOT_FOUND");
        }
        if (body.jobType === "sim.run") {
            const exe = body.params.sim_exe;
            if (typeof exe !== "string" ||
                !/^\.sigflow\/agent\/jobs\/sim\.build\/[A-Za-z0-9-]+\/artifacts\/.+$/.test(exe)) {
                throw new HttpError(
                    400,
                    "sim_exe 必须来自本工程的 sim.build 产物",
                    "INVALID_SIM_EXE",
                );
            }
        }

        const job = manager.submit({
            ownerId: userId,
            projectDir: projects.projectDir(userId, projectId),
            projectId,
            jobType: body.jobType,
            params: body.params,
            timeoutSec: body.timeoutSec,
        });
        return sendJson(res, 202, { id: job.id, state: job.state });
    }

    async function handleJob(req, res, url, job, rest, method) {
        if (rest === "" && method === "GET") {
            return sendJson(res, 200, {
                ...summarize(job),
                report: job.report,
                artifacts: (job.report?.artifacts ?? []).map((artifact) => ({
                    id: artifact.id,
                    path: artifact.path,
                    role: artifact.role,
                    sha256: artifact.sha256,
                    url: `/api/jobs/${job.id}/artifacts/${artifact.path.split("/").pop()}`,
                })),
            });
        }
        if (rest === "cancel" && method === "POST") {
            const canceled = manager.cancel(job.id);
            return sendJson(res, canceled ? 200 : 409, { canceled, state: job.state });
        }
        if (rest === "report" && method === "GET") {
            if (!job.report) throw new HttpError(404, "报告尚未生成", "NOT_FOUND");
            return sendJson(res, 200, job.report);
        }
        if (rest === "events" && method === "GET") {
            return streamEvents(req, res, url, job);
        }
        if (rest.startsWith("artifacts/") && method === "GET") {
            const name = rest.slice("artifacts/".length);
            if (!/^[A-Za-z0-9._-]+$/.test(name)) {
                throw new HttpError(400, "产物名不合法", "BAD_REQUEST");
            }
            const artifact = (job.report?.artifacts ?? []).find(
                (item) => item.path.split("/").pop() === name,
            );
            if (!artifact) throw new HttpError(404, "产物不存在", "NOT_FOUND");
            return serveAbsolute(res, artifactPath(job.jobDir, artifact.path));
        }
        throw new HttpError(404, "未找到", "NOT_FOUND");
    }

    function streamEvents(req, res, url, job) {
        res.writeHead(200, {
            "Content-Type": "text/event-stream; charset=utf-8",
            "Cache-Control": "no-cache, no-transform",
            Connection: "keep-alive",
            "X-Accel-Buffering": "no",
        });
        res.write(": connected\n\n");

        const after = Number.parseInt(url.searchParams.get("after") ?? "0", 10);
        let sent = Number.isFinite(after) && after > 0 ? after : 0;
        let timer = null;

        const stop = () => {
            if (timer) clearInterval(timer);
            timer = null;
        };

        const flush = () => {
            while (sent < job.events.length) {
                res.write(`id: ${sent}\ndata: ${JSON.stringify(job.events[sent])}\n\n`);
                sent += 1;
            }
            if (TERMINAL_STATES.has(job.state) && sent >= job.events.length) {
                res.write(`event: end\ndata: ${JSON.stringify({ state: job.state })}\n\n`);
                res.end();
                stop();
                return true;
            }
            return false;
        };

        if (flush()) return;
        timer = setInterval(flush, SSE_POLL_MS);
        req.on("close", stop);
        res.on("close", stop);
    }

    async function serveFile(res, root, relative) {
        const base = resolve(root);
        const target = resolve(base, relative);
        if (target !== base && !target.startsWith(base + sep)) {
            throw new HttpError(403, "路径越界", "FORBIDDEN");
        }
        try {
            const info = await stat(target);
            if (!info.isFile()) throw new HttpError(404, "未找到", "NOT_FOUND");
        } catch (error) {
            if (error instanceof HttpError) throw error;
            throw new HttpError(404, "未找到", "NOT_FOUND");
        }
        return serveAbsolute(res, target);
    }

    function serveAbsolute(res, target) {
        return new Promise((resolvePromise, reject) => {
            const stream = createReadStream(target);
            stream.on("open", () => {
                res.writeHead(200, {
                    "Content-Type": MIME_TYPES[extname(target)] ?? "application/octet-stream",
                    "Cache-Control": "no-cache",
                });
                stream.pipe(res);
            });
            stream.on("error", reject);
            stream.on("end", resolvePromise);
        });
    }

    function sendJson(res, status, payload) {
        if (res.headersSent) return;
        const body = JSON.stringify(payload);
        res.writeHead(status, {
            "Content-Type": "application/json; charset=utf-8",
            "Content-Length": Buffer.byteLength(body),
        });
        res.end(body);
    }

    function sendNoContent(res) {
        if (res.headersSent) return;
        res.writeHead(204);
        res.end();
    }

    return {
        setAgentManager(next) {
            agentManagerRef = next;
        },
        server,
        manager,
        users,
        sessions,
        projects,
        listen({ host = "127.0.0.1", port = 8080 } = {}) {
            return new Promise((resolvePromise, reject) => {
                server.once("error", reject);
                server.listen(port, host, () => {
                    server.off("error", reject);
                    resolvePromise(server.address());
                });
            });
        },
        async close() {
            await manager.shutdown();
            await new Promise((resolvePromise) => {
                server.close(() => resolvePromise());
                server.closeAllConnections?.();
            });
        },
    };
}

function summarize(job) {
    return {
        id: job.id,
        projectId: job.projectId,
        jobType: job.jobType,
        state: job.state,
        createdAt: job.createdAt,
        finishedAt: job.finishedAt,
        exitCode: job.exitCode,
        error: job.error,
        cancelReason: job.cancelReason,
        pid: job.pid,
        eventCount: job.events.length,
    };
}

async function readRawBody(req) {
    const chunks = [];
    for await (const chunk of req) chunks.push(chunk);
    return Buffer.concat(chunks).toString("utf8");
}

function readJson(req) {
    return new Promise((resolvePromise, reject) => {
        const chunks = [];
        let size = 0;
        req.on("data", (chunk) => {
            size += chunk.length;
            if (size > MAX_BODY_BYTES) {
                reject(new HttpError(413, "请求体过大", "PAYLOAD_TOO_LARGE"));
                req.destroy();
                return;
            }
            chunks.push(chunk);
        });
        req.on("error", reject);
        req.on("end", () => {
            if (size === 0) return reject(new HttpError(400, "请求体为空"));
            try {
                const value = JSON.parse(Buffer.concat(chunks).toString("utf8"));
                if (value === null || typeof value !== "object" || Array.isArray(value)) {
                    return reject(new HttpError(400, "请求体必须是 JSON 对象"));
                }
                return resolvePromise(value);
            } catch {
                return reject(new HttpError(400, "请求体不是合法 JSON"));
            }
        });
    });
}
