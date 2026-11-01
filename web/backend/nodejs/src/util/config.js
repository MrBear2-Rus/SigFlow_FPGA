// =============================================================================
// config.txt 解析与默认值
//
// 格式：`key = value`，`#` 注释，`[section]` 分组；结果按 "section.key" 扁平化。
// 只由 Node 主进程读取；C++ 侧通过命令行参数接收同样的值。
// =============================================================================

import { readFile } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url)); // backend/nodejs/src/util

/** web/ 根目录 */
export const WEB_ROOT = resolve(HERE, "..", "..", "..", "..");

/** 默认配置文件位置 */
export const CONFIG_PATH = join(WEB_ROOT, "config.txt");

/**
 * 解析 config.txt 文本。
 * @param {string} text
 * @returns {Record<string, string>} 形如 { "jobs.max_concurrent_global": "4" }
 */
export function parseConfig(text) {
    const result = {};
    let section = "";
    for (const rawLine of text.split(/\r?\n/)) {
        const line = rawLine.trim();
        if (line === "" || line.startsWith("#")) continue;

        const sectionMatch = /^\[([^\]]+)\]$/.exec(line);
        if (sectionMatch) {
            section = sectionMatch[1].trim();
            continue;
        }

        const eq = line.indexOf("=");
        if (eq < 0) continue; // 非法行直接忽略：配置不应让进程启动失败
        const key = line.slice(0, eq).trim();
        const value = line.slice(eq + 1).trim();
        if (key === "") continue;
        result[section ? `${section}.${key}` : key] = value;
    }
    return result;
}

/**
 * 读取并解析配置文件。
 * @param {string} [file]
 */
export async function loadConfig(file = CONFIG_PATH) {
    return parseConfig(await readFile(file, "utf8"));
}

/** 读取整数配置；非法值回退到 fallback 并交给调用方决定是否记录。 */
export function getNumber(config, key, fallback) {
    const raw = config[key];
    if (raw === undefined || raw === "") return fallback;
    const value = Number(raw);
    return Number.isFinite(value) ? value : fallback;
}

/** 读取字符串配置。 */
export function getString(config, key, fallback = "") {
    const raw = config[key];
    return raw === undefined || raw === "" ? fallback : raw;
}

/**
 * 解析 sigflow-tool 可执行文件位置。
 * 优先级：环境变量 SIGFLOW_TOOL_BIN → config.txt 的 tools.sigflow_tool → build/bin。
 */
export function resolveToolPath(config) {
    if (process.env.SIGFLOW_TOOL_BIN) return process.env.SIGFLOW_TOOL_BIN;
    const configured = getString(config, "tools.sigflow_tool");
    if (configured) return configured;
    return join(WEB_ROOT, "build", "bin", "sigflow-tool");
}
