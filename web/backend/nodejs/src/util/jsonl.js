// =============================================================================
// 有界 JSONL 解析
//
// C++ 侧每行输出一个 JSON 对象。本模块负责把字节流切成事件，并强制：
//   * 单行不超过 maxLineBytes（防止异常输出撑爆内存）；
//   * 非法 JSON 立即报错（不做容错猜测）；
//   * 结尾不完整的行由 finish() 单独交出，由调用方按失败处理。
// =============================================================================

const NEWLINE = 0x0a;

export class JsonlError extends Error {
    constructor(message) {
        super(message);
        this.name = "JsonlError";
    }
}

export class JsonlParser {
    /**
     * @param {{maxLineBytes?: number}} [options]
     */
    constructor({ maxLineBytes = 1024 * 1024 } = {}) {
        this.maxLineBytes = maxLineBytes;
        this.pending = Buffer.alloc(0);
        this.trailingLine = null;
        this.count = 0;
    }

    /**
     * 喂入一段数据，返回本次解析出的事件数组。
     * @param {Buffer} chunk
     * @returns {any[]}
     */
    feed(chunk) {
        const data = this.pending.length > 0 ? Buffer.concat([this.pending, chunk]) : chunk;
        const events = [];
        let start = 0;

        for (let i = 0; i < data.length; i += 1) {
            if (data[i] !== NEWLINE) continue;
            const line = data.subarray(start, i);
            start = i + 1;
            const event = this.#parseLine(line);
            if (event !== null) events.push(event);
        }

        this.pending = Buffer.from(data.subarray(start));
        if (this.pending.length > this.maxLineBytes) {
            throw new JsonlError(`JSONL 单行超过 ${this.maxLineBytes} 字节`);
        }
        return events;
    }

    /**
     * 流结束时调用：返回未换行的残留行（若有），由调用方决定是否视为错误。
     * @returns {string|null}
     */
    finish() {
        const rest = this.pending.toString("utf8").replace(/\r$/, "");
        this.pending = Buffer.alloc(0);
        if (rest.trim() === "") return null;
        this.trailingLine = rest;
        return rest;
    }

    #parseLine(buffer) {
        const line = buffer.toString("utf8").replace(/\r$/, "");
        if (line.trim() === "") return null;
        if (buffer.length > this.maxLineBytes) {
            throw new JsonlError(`JSONL 单行超过 ${this.maxLineBytes} 字节`);
        }
        let value;
        try {
            value = JSON.parse(line);
        } catch (error) {
            throw new JsonlError(`JSONL 不是合法 JSON: ${error.message}`);
        }
        if (value === null || typeof value !== "object" || Array.isArray(value)) {
            throw new JsonlError("JSONL 事件必须是 JSON 对象");
        }
        this.count += 1;
        return value;
    }
}
