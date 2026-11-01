// =============================================================================
// 会话：内存 token（进程重启即失效）
//
// Cookie 形态：HttpOnly + SameSite=Lax + Path=/。J3 不引入持久化会话，
// 重启后重新登录即可；后续若要"记住登录"，把 token 落盘即可。
// =============================================================================

import { randomBytes } from "node:crypto";

export const SESSION_COOKIE = "sigflow_session";

export class SessionStore {
    /** @param {{ttlSec?: number}} [options] */
    constructor({ ttlSec = 86400 } = {}) {
        this.ttlMs = Math.max(60, ttlSec) * 1000;
        this.sessions = new Map();
    }

    /** 新建会话，返回 token。 */
    create(userId) {
        this.#gc();
        const token = randomBytes(32).toString("hex");
        this.sessions.set(token, { userId, expiresAt: Date.now() + this.ttlMs });
        return token;
    }

    /** 返回 userId 或 null。 */
    resolve(token) {
        if (!token) return null;
        const session = this.sessions.get(token);
        if (!session) return null;
        if (session.expiresAt <= Date.now()) {
            this.sessions.delete(token);
            return null;
        }
        return session.userId;
    }

    destroy(token) {
        if (token) this.sessions.delete(token);
    }

    #gc() {
        const now = Date.now();
        for (const [token, session] of this.sessions) {
            if (session.expiresAt <= now) this.sessions.delete(token);
        }
    }
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
