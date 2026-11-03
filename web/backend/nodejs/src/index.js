// =============================================================================
// Node 主进程入口
//
// 当前阶段（J2）：HTTP / SSE / 产物下载已接入；前端与 Agent 托管尚未实现。
// 启动顺序：config -> jobs -> http ->（后续）wave -> agent
// =============================================================================

import { existsSync, mkdirSync } from "node:fs";
import { mkdir } from "node:fs/promises";
import { join } from "node:path";

import { AgentManager } from "./agent/manager.js";
import { AgentSupervisor } from "./agent/supervisor.js";
import { createApp } from "./http/server.js";
import { WEB_ROOT, getNumber, getString, loadConfig, resolveToolPath } from "./util/config.js";
import { createLogger } from "./util/log.js";

async function main() {
    const config = await loadConfig();
    const toolPath = resolveToolPath(config);
    if (!existsSync(toolPath)) {
        console.error(`[sigflow] 找不到 sigflow-tool：${toolPath}`);
        console.error("[sigflow] 先执行 cmake --build build/web，或设置 SIGFLOW_TOOL_BIN");
        process.exitCode = 1;
        return;
    }

    const logger = createLogger({ level: getString(config, "log.level", "info"), scope: "sigflow" });

    // config.txt 的 [tools] 覆盖映射成 C++ 侧识别的环境变量。
    const toolEnv = {};
    for (const [key, variable] of Object.entries({
        yosys: "SIGFLOW_YOSYS",
        nextpnr: "SIGFLOW_NEXTPNR",
        gowin_pack: "SIGFLOW_GOWIN_PACK",
        openfpgaloader: "SIGFLOW_OPENFPGALOADER",
        verilator: "SIGFLOW_VERILATOR",
    })) {
        const value = getString(config, `tools.${key}`);
        if (value) toolEnv[variable] = value;
    }

    const app = createApp({
        config,
        toolPath,
        maxGlobal: getNumber(config, "jobs.max_concurrent_global", 4),
        maxPerUser: getNumber(config, "jobs.max_concurrent_per_user", 2),
        defaultTimeoutSec: getNumber(config, "jobs.default_timeout_sec", 0),
        maxOutputBytes: getNumber(config, "jobs.max_output_bytes", 0),
        cancelGraceMs: getNumber(config, "jobs.cancel_grace_ms", 5000),
        sessionTtlSec: getNumber(config, "server.session_ttl_sec", 86400),
        maxFileBytes: getNumber(config, "files.max_bytes", 2 * 1024 * 1024),
        toolEnv,
        logger,
    });

    const host = getString(config, "server.listen_host", "127.0.0.1");
    const port = getNumber(config, "server.listen_port", 8080);
    const address = await app.listen({ host, port });
    console.log(`[sigflow] 监听 http://${host}:${address.port}`);
    console.log(`[sigflow] 工具: ${toolPath}`);

    let agentManager = null;
    const agentEnabled =
        process.env.SIGFLOW_AGENT_ENABLED === "1" ||
        getString(config, "agent.enabled", "false") === "true";
    if (agentEnabled) {
        const dataRoot = join(WEB_ROOT, "backend", "database", "agent");
        await mkdir(dataRoot, { recursive: true });
        agentManager = new AgentManager({
            createSupervisor: (userId, { gatewayToken, uiToken }) => {
                const dataDir = join(dataRoot, userId);
                mkdirSync(dataDir, { recursive: true });
                return new AgentSupervisor({
                    python: getString(config, "agent.python", "python3"),
                    module: getString(config, "agent.module", "sigflow_edu_agent"),
                    dataDir,
                    gatewayUrl: `http://127.0.0.1:${address.port}`,
                    gatewayToken,
                    uiToken,
                    logger,
                    backoffSec: getNumber(config, "agent.restart_backoff_sec", 1),
                    healthPollSec: getNumber(config, "agent.health_poll_sec", 5),
                });
            },
            idleMs: getNumber(config, "agent.idle_timeout_sec", 600) * 1000,
            maxInstances: getNumber(config, "agent.max_instances", 4),
            logger,
        });
        app.setAgentManager(agentManager);
        console.log("[sigflow] agent: 每用户 sidecar（按需启动）");
    }

    const shutdown = () => {
        console.log("[sigflow] 正在关闭…");
        Promise.resolve(agentManager?.stopAll())
            .catch(() => {})
            .finally(() => app.close())
            .finally(() => process.exit(0));
    };
    process.on("SIGINT", shutdown);
    process.on("SIGTERM", shutdown);
}

main().catch((error) => {
    console.error("[sigflow] 启动失败:", error);
    process.exitCode = 1;
});
