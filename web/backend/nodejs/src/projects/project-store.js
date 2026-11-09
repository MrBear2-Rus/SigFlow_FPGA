// =============================================================================
// 工程与文件（backend/userprojects/<userId>/<projectId>）
//
// 安全边界：
//   * 所有路径先按工程根解析，再逐段 lstat，遇到符号链接直接拒绝；
//   * 文件读写只允许文本类扩展名，且有大小上限；
//   * 文件树限制深度与条目数，避免超大目录拖垮接口。
// =============================================================================

import { lstat, mkdir, readFile, readdir, rename, rm, stat, writeFile } from "node:fs/promises";
import { dirname, extname, isAbsolute, join, relative, resolve, sep } from "node:path";

import { isValidId } from "../jobs/job-id.js";

const PROJECT_FILE = "sigflow.project";
const TEXT_EXTENSIONS = new Set([
    ".v", ".sv", ".vh", ".svh", ".cst", ".json", ".txt", ".md", ".mem", ".hex", ".sdc",
]);
const MAX_TREE_ENTRIES = 2000;
const MAX_DEPTH = 8;
const SKIP_DIRS = new Set([".sigflow", ".git", "node_modules"]);
export const TEMPLATES = ["blank", "sample"];
const SAMPLE_TOP = `// 示例：1 Hz 闪烁灯（27 MHz 时钟，Tang Nano 9K）
module top(input clk, output reg led);
    reg [24:0] count = 0;

    always @(posedge clk) begin
        if (count == 25'd27_000_000 - 1) begin
            count <= 0;
            led <= ~led;
        end else begin
            count <= count + 1;
        end
    end
endmodule
`;

export function projectError(status, code, message) {
    const error = new Error(message);
    error.status = status;
    error.code = code;
    return error;
}

export class ProjectStore {
    /** @param {{root: string, maxFileBytes?: number}} options */
    constructor({ root, maxFileBytes = 2 * 1024 * 1024 }) {
        this.root = resolve(root);
        this.maxFileBytes = maxFileBytes;
    }

    projectDir(userId, projectId) {
        if (!isValidId(userId)) throw projectError(400, "INVALID_USER_ID", "userId 不合法");
        if (!isValidId(projectId)) throw projectError(400, "INVALID_PROJECT_ID", "projectId 不合法");
        return this.#inside(this.root, join(userId, projectId));
    }

    async list(userId) {
        if (!isValidId(userId)) throw projectError(400, "INVALID_USER_ID", "userId 不合法");
        const userDir = this.#inside(this.root, userId);
        let entries;
        try {
            entries = await readdir(userDir, { withFileTypes: true });
        } catch (error) {
            if (error.code === "ENOENT") return [];
            throw error;
        }
        const projects = [];
        for (const entry of entries) {
            if (!entry.isDirectory()) continue;
            projects.push(await this.metadata(userId, entry.name));
        }
        return projects.sort((a, b) =>
            String(b.updatedAt ?? b.createdAt ?? "").localeCompare(
                String(a.updatedAt ?? a.createdAt ?? ""),
            ),
        );
    }

    async metadata(userId, projectId) {
        const dir = this.projectDir(userId, projectId);
        const file = join(dir, PROJECT_FILE);
        try {
            const parsed = JSON.parse(await readFile(file, "utf8"));
            if (!parsed.updatedAt) {
                try {
                    parsed.updatedAt = (await stat(file)).mtime.toISOString();
                } catch {
                    /* 忽略 */
                }
            }
            return { ...parsed, projectId };
        } catch (error) {
            if (error.code !== "ENOENT") throw error;
            return { projectId, schema: "sigflow.project.v1" };
        }
    }

    async create(userId, projectId, { name, template = "blank" } = {}) {
        if (!TEMPLATES.includes(template)) {
            throw projectError(400, "INVALID_TEMPLATE", `未知模板：${template}`);
        }
        const dir = this.projectDir(userId, projectId);
        await mkdir(join(dir), { recursive: true });
        try {
            await stat(join(dir, PROJECT_FILE));
            throw projectError(409, "PROJECT_EXISTS", `工程已存在：${projectId}`);
        } catch (error) {
            if (error.status) throw error;
            if (error.code !== "ENOENT") throw error;
        }
        const now = new Date().toISOString();
        const metadata = {
            schema: "sigflow.project.v1",
            projectId,
            name: typeof name === "string" && name.trim() !== "" ? name.trim() : projectId,
            template,
            createdAt: now,
            updatedAt: now,
        };
        await writeFile(join(dir, PROJECT_FILE), `${JSON.stringify(metadata, null, 2)}\n`);
        await mkdir(join(dir, "src"), { recursive: true });
        if (template === "sample") {
            await writeFile(join(dir, "src", "top.v"), SAMPLE_TOP);
        }
        return { ...metadata };
    }

    async exists(userId, projectId) {
        try {
            const info = await stat(this.projectDir(userId, projectId));
            return info.isDirectory();
        } catch {
            return false;
        }
    }

    async remove(userId, projectId) {
        const dir = this.projectDir(userId, projectId);
        if (!(await this.exists(userId, projectId))) {
            throw projectError(404, "PROJECT_NOT_FOUND", `工程不存在：${projectId}`);
        }
        await rm(dir, { recursive: true, force: false });
    }

    /** 返回工程内文件列表（相对路径），跳过 .sigflow/.git 等。 */
    async listFiles(userId, projectId) {
        const dir = this.projectDir(userId, projectId);
        if (!(await this.exists(userId, projectId))) {
            throw projectError(404, "PROJECT_NOT_FOUND", `工程不存在：${projectId}`);
        }
        const files = [];
        let truncated = false;

        const walk = async (current, depth) => {
            if (depth > MAX_DEPTH) return;
            let entries;
            try {
                entries = await readdir(current, { withFileTypes: true });
            } catch {
                return;
            }
            for (const entry of entries.sort((a, b) => a.name.localeCompare(b.name))) {
                if (files.length >= MAX_TREE_ENTRIES) {
                    truncated = true;
                    return;
                }
                if (SKIP_DIRS.has(entry.name) || entry.name === PROJECT_FILE) continue;
                if (entry.name.endsWith(".tmp")) continue;
                const absolute = join(current, entry.name);
                const rel = relative(dir, absolute).split(sep).join("/");
                if (entry.isDirectory()) {
                    files.push({ path: rel, type: "dir" });
                    await walk(absolute, depth + 1);
                } else if (entry.isFile()) {
                    let size = 0;
                    try {
                        size = (await stat(absolute)).size;
                    } catch {
                        /* 忽略并发删除 */
                    }
                    files.push({ path: rel, type: "file", size });
                }
            }
        };

        await walk(dir, 0);
        return { files, truncated };
    }

    /** 返回工程内文本文件的绝对路径（供 C++ 分析命令直接读取）。 */
    async absoluteFile(userId, projectId, relPath) {
        return this.#textFile(userId, projectId, relPath, { mustExist: true });
    }

    /** 列出 sim.run 产物中的 VCD（相对工程根，POSIX 分隔）。 */
    async listWaveArtifacts(userId, projectId) {
        if (!(await this.exists(userId, projectId))) {
            throw projectError(404, "PROJECT_NOT_FOUND", `工程不存在：${projectId}`);
        }
        const projectDir = this.projectDir(userId, projectId);
        const base = join(projectDir, ".sigflow", "agent", "jobs", "sim.run");
        const found = [];
        const walk = async (dir, depth) => {
            if (depth > 4) return;
            let entries;
            try {
                entries = await readdir(dir, { withFileTypes: true });
            } catch {
                return;
            }
            for (const entry of entries) {
                const full = join(dir, entry.name);
                if (entry.isDirectory()) await walk(full, depth + 1);
                else if (entry.isFile() && entry.name.endsWith(".vcd")) {
                    found.push(relative(projectDir, full).split(sep).join("/"));
                }
            }
        };
        await walk(base, 0);
        return found.sort();
    }

    /** 返回工程内任意已存在产物（如 .vcd）的绝对路径；不做扩展名限制。 */
    async artifactFile(userId, projectId, relPath) {
        if (typeof relPath !== "string" || relPath === "" || isAbsolute(relPath)) {
            throw projectError(400, "INVALID_PATH", "路径必须是非空相对路径");
        }
        if (!(await this.exists(userId, projectId))) {
            throw projectError(404, "PROJECT_NOT_FOUND", `工程不存在：${projectId}`);
        }
        const dir = this.projectDir(userId, projectId);
        const target = this.#inside(dir, relPath);
        await this.#assertNoSymlink(dir, target);
        let info;
        try {
            info = await lstat(target);
        } catch {
            throw projectError(404, "FILE_NOT_FOUND", `文件不存在：${relPath}`);
        }
        if (!info.isFile()) throw projectError(400, "NOT_A_FILE", `不是普通文件：${relPath}`);
        return target;
    }

    /** 画布布局：<project>/.sigflow/canvas.json */
    async readCanvas(userId, projectId) {
        const dir = this.projectDir(userId, projectId);
        if (!(await this.exists(userId, projectId))) {
            throw projectError(404, "PROJECT_NOT_FOUND", `工程不存在：${projectId}`);
        }
        try {
            const text = await readFile(join(dir, ".sigflow", "canvas.json"), "utf8");
            const parsed = JSON.parse(text);
            return parsed && typeof parsed === "object" ? parsed : { schema: "sigflow.canvas.v1", elements: [], wires: [] };
        } catch (error) {
            if (error.code === "ENOENT") {
                return { schema: "sigflow.canvas.v1", elements: [], wires: [] };
            }
            throw projectError(500, "CANVAS_CORRUPT", "画布文件损坏");
        }
    }

    async writeCanvas(userId, projectId, layout) {
        const dir = this.projectDir(userId, projectId);
        if (!(await this.exists(userId, projectId))) {
            throw projectError(404, "PROJECT_NOT_FOUND", `工程不存在：${projectId}`);
        }
        if (layout === null || typeof layout !== "object" || Array.isArray(layout)) {
            throw projectError(400, "INVALID_CANVAS", "画布数据必须是对象");
        }
        if (!Array.isArray(layout.elements) || !Array.isArray(layout.wires)) {
            throw projectError(400, "INVALID_CANVAS", "画布需要 elements 与 wires 数组");
        }
        const text = `${JSON.stringify(layout, null, 2)}\n`;
        if (Buffer.byteLength(text) > this.maxFileBytes) {
            throw projectError(413, "FILE_TOO_LARGE", "画布数据过大");
        }
        const target = join(dir, ".sigflow", "canvas.json");
        await mkdir(join(dir, ".sigflow"), { recursive: true });
        await this.#assertNoSymlink(dir, target);
        const temporary = `${target}.tmp`;
        await writeFile(temporary, text);
        await rename(temporary, target);
        await this.#touch(userId, projectId);
        return { elements: layout.elements.length, wires: layout.wires.length };
    }

    async readFile(userId, projectId, relPath) {
        const target = await this.#textFile(userId, projectId, relPath, { mustExist: true });
        const info = await stat(target);
        if (info.size > this.maxFileBytes) {
            throw projectError(413, "FILE_TOO_LARGE", `文件超过 ${this.maxFileBytes} 字节`);
        }
        return readFile(target, "utf8");
    }

    async writeFile(userId, projectId, relPath, content) {
        if (typeof content !== "string") {
            throw projectError(400, "INVALID_CONTENT", "content 必须是字符串");
        }
        if (Buffer.byteLength(content) > this.maxFileBytes) {
            throw projectError(413, "FILE_TOO_LARGE", `文件超过 ${this.maxFileBytes} 字节`);
        }
        const target = await this.#textFile(userId, projectId, relPath, { mustExist: false });
        await mkdir(resolve(target, ".."), { recursive: true });
        const temporary = `${target}.tmp`;
        await writeFile(temporary, content);
        await rename(temporary, target);
        await this.#touch(userId, projectId);
        return { path: relPath, size: Buffer.byteLength(content) };
    }

    /** 更新工程的 updatedAt（文件或画布变更后调用）。 */
    async #touch(userId, projectId) {
        try {
            const file = join(this.projectDir(userId, projectId), PROJECT_FILE);
            const metadata = JSON.parse(await readFile(file, "utf8"));
            metadata.updatedAt = new Date().toISOString();
            await writeFile(file, `${JSON.stringify(metadata, null, 2)}\n`);
        } catch {
            /* 元数据缺失时忽略 */
        }
    }

    async moveFile(userId, projectId, from, to) {
        if (from === to) return { from, to };
        const source = await this.#textFile(userId, projectId, from, { mustExist: true });
        const target = await this.#textFile(userId, projectId, to, { mustExist: false });
        const info = await lstat(source);
        if (info.isDirectory()) {
            throw projectError(400, "IS_DIRECTORY", "暂不支持移动目录");
        }
        try {
            await lstat(target);
            throw projectError(409, "FILE_EXISTS", `目标已存在：${to}`);
        } catch (error) {
            if (error.code !== "ENOENT") throw error;
        }
        await mkdir(dirname(target), { recursive: true });
        await rename(source, target);
        await this.#touch(userId, projectId);
        return { from, to };
    }

    async removeFile(userId, projectId, relPath) {
        const target = await this.#textFile(userId, projectId, relPath, { mustExist: true });
        const info = await lstat(target);
        if (info.isDirectory()) {
            throw projectError(400, "IS_DIRECTORY", "暂不支持删除目录");
        }
        await rm(target);
        await this.#touch(userId, projectId);
    }

    async #textFile(userId, projectId, relPath, { mustExist }) {
        if (typeof relPath !== "string" || relPath === "" || isAbsolute(relPath)) {
            throw projectError(400, "INVALID_PATH", "路径必须是非空相对路径");
        }
        const dir = this.projectDir(userId, projectId);
        if (!(await this.exists(userId, projectId))) {
            throw projectError(404, "PROJECT_NOT_FOUND", `工程不存在：${projectId}`);
        }
        const target = this.#inside(dir, relPath);

        if (!TEXT_EXTENSIONS.has(extname(target))) {
            throw projectError(400, "UNSUPPORTED_FILE", `不支持的文件类型：${extname(target)}`);
        }

        await this.#assertNoSymlink(dir, target);
        if (mustExist) {
            try {
                await lstat(target);
            } catch {
                throw projectError(404, "FILE_NOT_FOUND", `文件不存在：${relPath}`);
            }
        }
        return target;
    }

    #inside(base, rel) {
        const target = resolve(base, rel);
        const baseResolved = resolve(base);
        if (target !== baseResolved && !target.startsWith(baseResolved + sep)) {
            throw projectError(400, "PATH_ESCAPE", "路径越界");
        }
        return target;
    }

    async #assertNoSymlink(base, target) {
        const rel = relative(resolve(base), resolve(target));
        let current = resolve(base);
        for (const part of rel.split(sep)) {
            if (part === "") continue;
            current = join(current, part);
            try {
                const info = await lstat(current);
                if (info.isSymbolicLink()) {
                    throw projectError(400, "SYMLINK_FORBIDDEN", "不允许符号链接");
                }
            } catch (error) {
                if (error.status) throw error;
                if (error.code === "ENOENT") break;
                throw error;
            }
        }
    }
}
