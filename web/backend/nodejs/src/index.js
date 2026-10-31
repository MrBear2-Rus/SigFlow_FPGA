// =============================================================================
// Node 主进程入口（骨架）
//
// 这是服务器上唯一的常驻进程。当前只做启动前的自检：读 config.txt、确认
// 数据目录存在。各模块装配顺序见 README「具体架构 / 4. Node 侧」。
//
// 依赖顺序（实现时按此装配）：
//   config -> projects -> jobs/spawn -> devices -> wave -> http -> agent
// =============================================================================

import { readFile } from "node:fs/promises";
import { existsSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));      // backend/nodejs/src
const WEB_ROOT = resolve(HERE, "..", "..", "..");          // web/
const CONFIG_PATH = join(WEB_ROOT, "config.txt");

/**
 * 解析 config.txt：`key = value`，`#` 注释，`[section]` 分组。
 * 返回 { "section.key": "value" } 形式的扁平表。
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
    if (eq < 0) continue;
    const key = line.slice(0, eq).trim();
    const value = line.slice(eq + 1).trim();
    result[section ? `${section}.${key}` : key] = value;
  }
  return result;
}

async function main() {
  if (!existsSync(CONFIG_PATH)) {
    console.error(`[sigflow] 缺少配置文件: ${CONFIG_PATH}`);
    process.exitCode = 2;
    return;
  }
  const config = parseConfig(await readFile(CONFIG_PATH, "utf8"));
  console.log(`[sigflow] 配置已读取: ${CONFIG_PATH} (${Object.keys(config).length} 项)`);

  // 迁移中：业务模块尚未接入。
  console.error("[sigflow] HTTP / Job 调度 / 波形 worker / Agent 托管尚未实现");
  process.exitCode = 3;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().catch((error) => {
    console.error("[sigflow] 启动失败:", error);
    process.exitCode = 1;
  });
}
