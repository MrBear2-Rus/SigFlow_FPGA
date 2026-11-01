// =============================================================================
// 极简日志：统一前缀与级别，避免各模块各写一套 console.log。
// =============================================================================

const LEVELS = { debug: 10, info: 20, warn: 30, error: 40 };

/**
 * @param {{level?: keyof LEVELS, scope?: string, sink?: (line: string) => void}} [options]
 */
export function createLogger({ level = "info", scope = "sigflow", sink } = {}) {
    const threshold = LEVELS[level] ?? LEVELS.info;

    const write = (name, args) => {
        if ((LEVELS[name] ?? 0) < threshold) return;
        const stamp = new Date().toISOString();
        const line = `[${stamp}] [${name}] [${scope}]`;
        if (sink) {
            sink(`${line} ${args.map(String).join(" ")}`);
        } else {
            console.error(line, ...args);
        }
    };

    return {
        debug: (...args) => write("debug", args),
        info: (...args) => write("info", args),
        warn: (...args) => write("warn", args),
        error: (...args) => write("error", args),
        child: (childScope) => createLogger({ level, scope: childScope, sink }),
    };
}
