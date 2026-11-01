// =============================================================================
// 用户表：backend/database/users.json
//
// 只存 salt 与 scrypt 派生值，不存明文密码。文件用临时文件 + rename 原子替换。
// 这是 J3 的最小实现；后续若换数据库，只需替换本模块。
// =============================================================================

import { randomBytes, scrypt, timingSafeEqual } from "node:crypto";
import { mkdir, readFile, rename, writeFile } from "node:fs/promises";
import { dirname } from "node:path";

const SCRYPT = { N: 16384, r: 8, p: 1, keylen: 32 };
const USER_ID_PATTERN = /^[A-Za-z0-9._-]{1,64}$/;
const MIN_PASSWORD_LENGTH = 8;

function fail(status, code, message) {
    const error = new Error(message);
    error.status = status;
    error.code = code;
    return error;
}

function derive(password, salt) {
    return new Promise((resolve, reject) => {
        scrypt(password, salt, SCRYPT.keylen, { N: SCRYPT.N, r: SCRYPT.r, p: SCRYPT.p }, (error, key) =>
            error ? reject(error) : resolve(key),
        );
    });
}

export class UserStore {
    /** @param {{file: string}} options */
    constructor({ file }) {
        this.file = file;
        this.users = null;
    }

    static validateUserId(userId) {
        if (typeof userId !== "string" || !USER_ID_PATTERN.test(userId)) {
            throw fail(400, "INVALID_USER_ID", "userId 需为 1–64 位字母数字或 . _ -");
        }
        return userId;
    }

    async list() {
        const users = await this.#load();
        return Object.keys(users);
    }

    async exists(userId) {
        const users = await this.#load();
        return Object.hasOwn(users, userId);
    }

    /** 创建用户；已存在时抛出 409。 */
    async create(userId, password) {
        UserStore.validateUserId(userId);
        if (typeof password !== "string" || password.length < MIN_PASSWORD_LENGTH) {
            throw fail(400, "WEAK_PASSWORD", `密码至少 ${MIN_PASSWORD_LENGTH} 位`);
        }
        const users = await this.#load();
        if (Object.hasOwn(users, userId)) {
            throw fail(409, "USER_EXISTS", `用户已存在：${userId}`);
        }
        const salt = randomBytes(16).toString("hex");
        const hash = (await derive(password, salt)).toString("hex");
        users[userId] = { salt, hash, createdAt: new Date().toISOString() };
        await this.#save(users);
        return { userId };
    }

    /** 校验密码；用户不存在与密码错误返回同一种结果，避免枚举用户。 */
    async verify(userId, password) {
        if (typeof userId !== "string" || typeof password !== "string") return false;
        const users = await this.#load();
        const record = users[userId];
        if (!record) return false;
        const expected = Buffer.from(record.hash, "hex");
        const actual = await derive(password, record.salt);
        return expected.length === actual.length && timingSafeEqual(expected, actual);
    }

    async #load() {
        if (this.users) return this.users;
        try {
            const parsed = JSON.parse(await readFile(this.file, "utf8"));
            this.users = parsed?.users && typeof parsed.users === "object" ? parsed.users : {};
        } catch (error) {
            if (error.code !== "ENOENT") throw error;
            this.users = {};
        }
        return this.users;
    }

    async #save(users) {
        await mkdir(dirname(this.file), { recursive: true });
        const temporary = `${this.file}.tmp`;
        await writeFile(temporary, `${JSON.stringify({ version: 1, users }, null, 2)}\n`, {
            mode: 0o600,
        });
        await rename(temporary, this.file);
        this.users = users;
    }
}
