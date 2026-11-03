// 目标板数据：backend/static/target-profiles/<id>.json 与 <id>.pins.json

import { readFile, readdir } from "node:fs/promises";
import { join } from "node:path";

const ID_PATTERN = /^[A-Za-z0-9._-]{1,64}$/;

export class TargetStore {
    constructor({ root }) {
        this.root = root;
    }

    /** 列表：只返回 profile 摘要，不返回整份引脚表。 */
    async list() {
        let entries;
        try {
            entries = await readdir(this.root);
        } catch {
            return [];
        }
        const targets = [];
        for (const name of entries) {
            if (!name.endsWith(".json") || name.endsWith(".pins.json")) continue;
            const id = name.slice(0, -".json".length);
            if (!ID_PATTERN.test(id)) continue;
            try {
                const profile = JSON.parse(await readFile(join(this.root, name), "utf8"));
                targets.push({
                    id: profile.id ?? id,
                    displayName: profile.display_name ?? id,
                    device: profile.nextpnr?.device ?? "",
                    family: profile.nextpnr?.family ?? "",
                    version: profile.version ?? "",
                });
            } catch {
                /* 跳过损坏的 profile */
            }
        }
        return targets.sort((a, b) => a.id.localeCompare(b.id));
    }

    /** profile + pins 全文。 */
    async get(id) {
        if (!ID_PATTERN.test(id)) {
            throw Object.assign(new Error("目标板 id 不合法"), { status: 400, code: "INVALID_TARGET" });
        }
        let profile;
        let pins;
        try {
            profile = JSON.parse(await readFile(join(this.root, `${id}.json`), "utf8"));
            pins = JSON.parse(await readFile(join(this.root, `${id}.pins.json`), "utf8"));
        } catch {
            throw Object.assign(new Error(`目标板不存在：${id}`), {
                status: 404,
                code: "TARGET_NOT_FOUND",
            });
        }
        return { profile, pins };
    }

    pinsPath(id) {
        if (!ID_PATTERN.test(id)) {
            throw Object.assign(new Error("目标板 id 不合法"), { status: 400, code: "INVALID_TARGET" });
        }
        return join(this.root, `${id}.pins.json`);
    }
}
