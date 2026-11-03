// =============================================================================
// 每用户一个 Python sidecar（决策 D-17）
//
//   forUser(userId) 惰性启动；空闲回收；超出上限时淘汰最久未用的实例。
//   每个实例持有独立的 gateway_token / ui_token，token 反查用户用于网关路由。
// =============================================================================

import { randomBytes } from "node:crypto";

export class AgentManager {
    constructor({ createSupervisor, idleMs = 10 * 60 * 1000, maxInstances = 4, logger } = {}) {
        this.createSupervisor = createSupervisor;
        this.idleMs = idleMs;
        this.maxInstances = maxInstances;
        this.logger = logger;
        this.entries = new Map(); // userId -> { supervisor, gatewayToken, uiToken, lastUsed }
        this.tokens = new Map(); // gatewayToken -> userId

        this.reaper = setInterval(() => this.#reap(), Math.min(60_000, Math.max(10_000, idleMs / 2)));
        this.reaper.unref?.();
    }

    get size() {
        return this.entries.size;
    }

    entry(userId) {
        return this.entries.get(userId);
    }

    /** 按用户获取（必要时启动）实例。 */
    async forUser(userId) {
        const existing = this.entries.get(userId);
        if (existing?.supervisor.ready) {
            existing.lastUsed = Date.now();
            return existing;
        }
        if (existing) {
            this.tokens.delete(existing.gatewayToken);
            this.entries.delete(userId);
        }

        if (this.entries.size >= this.maxInstances) {
            const [oldest] = [...this.entries.entries()].sort(
                (a, b) => a[1].lastUsed - b[1].lastUsed,
            );
            if (oldest) {
                this.logger?.warn?.(`Agent 实例超限，回收 ${oldest[0]}`);
                await this.stopUser(oldest[0]);
            }
        }

        const gatewayToken = randomBytes(24).toString("hex");
        const uiToken = randomBytes(24).toString("hex");
        const supervisor = this.createSupervisor(userId, { gatewayToken, uiToken });
        const entry = { supervisor, gatewayToken, uiToken, lastUsed: Date.now() };
        this.entries.set(userId, entry);
        this.tokens.set(gatewayToken, userId);

        try {
            await supervisor.start();
            return entry;
        } catch (error) {
            this.entries.delete(userId);
            this.tokens.delete(gatewayToken);
            throw error;
        }
    }

    /** 网关鉴权：token -> { userId, entry }。 */
    resolveToken(token) {
        const userId = this.tokens.get(token);
        if (!userId) return null;
        const entry = this.entries.get(userId);
        if (!entry) return null;
        entry.lastUsed = Date.now();
        return { userId, entry };
    }

    async stopUser(userId) {
        const entry = this.entries.get(userId);
        if (!entry) return;
        this.entries.delete(userId);
        this.tokens.delete(entry.gatewayToken);
        await entry.supervisor.stop().catch(() => {});
    }

    async stopAll() {
        clearInterval(this.reaper);
        await Promise.all([...this.entries.keys()].map((userId) => this.stopUser(userId)));
    }

    #reap() {
        const now = Date.now();
        for (const [userId, entry] of this.entries) {
            if (now - entry.lastUsed > this.idleMs) {
                this.logger?.info?.(`Agent 空闲回收：${userId}`);
                void this.stopUser(userId);
            }
        }
    }
}
