// 每用户一个 wave worker；空闲回收，上限淘汰。

import { WaveWorker } from "./worker.js";

export class WaveManager {
    constructor({ toolPath, idleMs = 10 * 60 * 1000, maxInstances = 4, logger } = {}) {
        this.toolPath = toolPath;
        this.idleMs = idleMs;
        this.maxInstances = maxInstances;
        this.logger = logger;
        this.entries = new Map(); // userId -> { worker, lastUsed }
        this.reaper = setInterval(() => this.#reap(), 60_000);
        this.reaper.unref?.();
    }

    async forUser(userId) {
        const existing = this.entries.get(userId);
        if (existing) {
            existing.lastUsed = Date.now();
            return existing.worker;
        }
        if (this.entries.size >= this.maxInstances) {
            const [oldest] = [...this.entries.entries()].sort((a, b) => a[1].lastUsed - b[1].lastUsed);
            if (oldest) await this.stopUser(oldest[0]);
        }
        const worker = new WaveWorker({ toolPath: this.toolPath, logger: this.logger });
        this.entries.set(userId, { worker, lastUsed: Date.now() });
        return worker;
    }

    async stopUser(userId) {
        const entry = this.entries.get(userId);
        if (!entry) return;
        this.entries.delete(userId);
        await entry.worker.stop().catch(() => {});
    }

    async stopAll() {
        clearInterval(this.reaper);
        await Promise.all([...this.entries.keys()].map((userId) => this.stopUser(userId)));
    }

    #reap() {
        const now = Date.now();
        for (const [userId, entry] of this.entries) {
            if (now - entry.lastUsed > this.idleMs) {
                this.logger?.info?.(`wave worker 空闲回收：${userId}`);
                void this.stopUser(userId);
            }
        }
    }
}
