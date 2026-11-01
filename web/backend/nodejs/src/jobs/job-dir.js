// =============================================================================
// Job 目录
//
// 契约（contracts/run-v1 + P2 交接）：Node 必须在提交前创建**空目录**
//   <project>/.sigflow/agent/jobs/<jobType>/<jobId>
// 且路径必须严格等于 CLI 推导出的位置，否则 CLI 会以 "Job path does not match
// identity" 拒绝。目录不可复用，重试必须换新 jobId。
// =============================================================================

import { mkdir, readdir } from "node:fs/promises";
import { isAbsolute, join, relative, resolve, sep } from "node:path";

import { assertValidId } from "./job-id.js";

/** 目前 C++ 侧已实现的 jobType；其余会在 CLI 返回 3（未实现）。 */
export const KNOWN_JOB_TYPES = [
    "synth",
    "pnr",
    "pack",
    "flash",
    "sim.build",
    "sim.run",
];

/**
 * 在工程内创建并返回空的 Job 目录。
 * @param {string} projectDir 工程根目录（绝对或相对路径）
 * @param {string} jobType
 * @param {string} jobId
 * @returns {Promise<string>} Job 目录绝对路径
 */
export async function prepareJobDir(projectDir, jobType, jobId) {
    if (!KNOWN_JOB_TYPES.includes(jobType)) {
        throw new Error(`未知 jobType：${jobType}`);
    }
    assertValidId(jobId, "jobId");

    const root = resolve(projectDir);
    const jobDir = join(root, ".sigflow", "agent", "jobs", jobType, jobId);

    // 防止 jobType 或 jobId 通过分隔符逃出工程目录。
    const rel = relative(root, jobDir);
    if (rel === "" || rel.startsWith("..") || isAbsolute(rel)) {
        throw new Error(`Job 目录越界：${jobDir}`);
    }
    if (!jobDir.startsWith(root + sep) && jobDir !== root) {
        throw new Error(`Job 目录不在工程内：${jobDir}`);
    }

    await mkdir(jobDir, { recursive: true });
    const entries = await readdir(jobDir);
    if (entries.length > 0) {
        throw new Error(`Job 目录非空，拒绝复用：${jobDir}`);
    }
    return jobDir;
}

/** Job 报告路径（与 CLI 约定一致）。 */
export function reportPath(jobDir) {
    return join(jobDir, "reports", "report.json");
}

/** 产物路径（Artifact.path 是相对 Job 目录的路径）。 */
export function artifactPath(jobDir, relativePath) {
    const value = resolve(jobDir, relativePath);
    const rel = relative(resolve(jobDir), value);
    if (rel.startsWith("..") || isAbsolute(rel)) {
        throw new Error(`产物路径越界：${relativePath}`);
    }
    return value;
}
