// =============================================================================
// 会话：内存 token（进程重启即失效）
//
// Cookie 形态：HttpOnly + SameSite=Lax + Path=/。J3 不引入持久化会话，
// 重启后重新登录即可；后续若要"记住登录"，把 token 落盘即可。
// =============================================================================

import { createHash, randomBytes } from "node:crypto";
import { existsSync, readFileSync, renameSync, writeFileSync } from "node:fs";

export const SESSION_COOKIE = "sigflow_session";

export class SessionStore {
    /** @param {{ttlSec?: number, file?: string}} [options] */
    constructor({ ttlSec = 86400, file = "" } = {}) {
        this.ttlMs = Math.max(60, ttlSec) * 1000;
        this.file = file;
        this.sessions = new Map();
        this.#load();
    }

    /** 新建会话，返回 token。 */
    create(userId) {
        this.#gc();
        const token = randomBytes(32).toString("hex");
        this.sessions.set(hashToken(token), { userId, expiresAt: Date.now() + this.ttlMs });
        this.#persist();
        return token;
    }

    /** 返回 userId 或 null。 */
    resolve(token) {
        if (!token) return null;
        const key = hashToken(token);
        const session = this.sessions.get(key);
        if (!session) return null;
        if (session.expiresAt <= Date.now()) {
            this.sessions.delete(key);
            this.#persist();
            return null;
        }
        return session.userId;
    }

    destroy(token) {
        if (!token) return;
        if (this.sessions.delete(hashToken(token))) this.#persist();
    }

    #gc() {
        const now = Date.now();
        let changed = false;
        for (const [key, session] of this.sessions) {
            if (session.expiresAt <= now) {
                this.sessions.delete(key);
                changed = true;
            }
        }
        if (changed) this.#persist();
    }

    #load() {
        if (!this.file || !existsSync(this.file)) return;
        try {
            const data = JSON.parse(readFileSync(this.file, "utf8"));
            for (const [key, session] of Object.entries(data.sessions ?? {})) {
                if (Number(session.expiresAt) > Date.now()) this.sessions.set(key, session);
            }
        } catch {
            /* 文件损坏时从空会话开始 */
        }
    }

    #persist() {
        if (!this.file) return;
        const temporary = `${this.file}.tmp`;
        const payload = { version: 1, sessions: Object.fromEntries(this.sessions) };
        try {
            writeFileSync(temporary, `${JSON.stringify(payload)}\n`);
            renameSync(temporary, this.file);
        } catch {
            /* 持久化失败不影响内存会话 */
        }
    }
}

function hashToken(token) {
    return createHash("sha256").update(token).digest("hex");
}

/** 从 Cookie 头里取会话 token。 */
export function readSessionCookie(header) {
    if (typeof header !== "string" || header === "") return null;
    for (const part of header.split(";")) {
        const [name, ...rest] = part.trim().split("=");
        if (name === SESSION_COOKIE) return decodeURIComponent(rest.join("="));
    }
    return null;
}

/** 生成 Set-Cookie 值。 */
export function sessionCookie(token, { maxAgeSec } = {}) {
    const parts = [`${SESSION_COOKIE}=${token}`, "HttpOnly", "SameSite=Lax", "Path=/"];
    if (typeof maxAgeSec === "number") parts.push(`Max-Age=${maxAgeSec}`);
    return parts.join("; ");
}
