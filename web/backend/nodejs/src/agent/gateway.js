// =============================================================================
// Python Agent 需要的 Gateway 兼容路由（Node 扮演宿主）
//
//   GET /api/v1/health
//   GET /api/v1/projects/{id}/context
//   GET /api/v1/jobs/{id}/report
//
// 语义见 README 4.3：Bearer gateway_token、edu.api.v1 信封、2 MiB 上限。
// =============================================================================

import { randomUUID } from "node:crypto";

const SCHEMA = "edu.api.v1";
const MAX_ENVELOPE_BYTES = 2 * 1024 * 1024;

function envelope(data) {
    return JSON.stringify({
        schema_version: SCHEMA,
        request_id: randomUUID(),
        trace_id: randomUUID(),
        data,
    });
}

function failure(res, status, code, message) {
    const body = JSON.stringify({
        schema_version: SCHEMA,
        request_id: randomUUID(),
        trace_id: randomUUID(),
        error: { code, message },
    });
    res.writeHead(status, {
        "content-type": "application/json; charset=utf-8",
        "content-length": Buffer.byteLength(body),
    });
    res.end(body);
}

function ok(res, status, data) {
    const body = envelope(data);
    if (Buffer.byteLength(body) > MAX_ENVELOPE_BYTES) {
        return failure(res, 500, "RESOURCE_EXHAUSTED", "gateway response exceeds 2 MiB");
    }
    res.writeHead(status, {
        "content-type": "application/json; charset=utf-8",
        "content-length": Buffer.byteLength(body),
    });
    res.end(body);
}

/**
 * @param {{ agentManager: import("./manager.js").AgentManager, projects: object, jobs: object }} deps
 * @returns {(req, res, url) => Promise<boolean>} 返回 false 表示不是网关路由
 */
export function createAgentGateway({ agentManager, projects, jobs }) {
    return async function handle(req, res, url) {
        if (!url.pathname.startsWith("/api/v1/")) return false;

        const auth = req.headers.authorization ?? "";
        const match = /^Bearer (.+)$/.exec(auth);
        const resolved = match ? agentManager.resolveToken(match[1]) : null;
        if (!resolved) {
            failure(res, 401, "POLICY_DENIED", "invalid gateway token");
            return true;
        }
        const { userId } = resolved;

        if (url.pathname === "/api/v1/health" && req.method === "GET") {
            ok(res, 200, { status: "ok", user: userId });
            return true;
        }

        const contextMatch = /^\/api\/v1\/projects\/([^/]+)\/context$/.exec(url.pathname);
        if (contextMatch && req.method === "GET") {
            const projectId = decodeURIComponent(contextMatch[1]);
            try {
                const metadata = await projects.metadata(userId, projectId);
                const tree = await projects.listFiles(userId, projectId);
                ok(res, 200, {
                    project_id: projectId,
                    name: metadata.name ?? projectId,
                    revision: metadata.updatedAt ?? metadata.createdAt ?? "1",
                    files: (tree.files ?? [])
                        .filter((entry) => entry.type === "file")
                        .map((entry) => entry.path),
                });
            } catch {
                failure(res, 404, "NOT_FOUND", "project not found");
            }
            return true;
        }

        const reportMatch = /^\/api\/v1\/jobs\/([^/]+)\/report$/.exec(url.pathname);
        if (reportMatch && req.method === "GET") {
            const job = jobs.get(decodeURIComponent(reportMatch[1]));
            if (!job || job.ownerId !== userId) {
                failure(res, 404, "NOT_FOUND", "job not found");
            } else if (!job.report) {
                failure(res, 404, "NOT_FOUND", "report not ready");
            } else {
                ok(res, 200, job.report);
            }
            return true;
        }

        failure(res, 404, "NOT_FOUND", "unknown gateway route");
        return true;
    };
}
