// =============================================================================
// Job / 工程 ID
//
// C++ 侧 RunCommand 的规则：1–128 位 ASCII 字母、数字、`.`、`_`、`-`，
// 且不允许单独的 "." 或 ".."。这里保持一致，避免先生成再被 CLI 拒绝。
// =============================================================================

import { randomUUID } from "node:crypto";

const ID_PATTERN = /^[A-Za-z0-9._-]{1,128}$/;

/** 生成一个新的 Job ID（UUID 只含字母数字与 `-`，符合规则）。 */
export function newJobId() {
    return randomUUID();
}

/** 判断字符串是否是 C++ 侧认可的 ID。 */
export function isValidId(value) {
    return typeof value === "string" && value !== "." && value !== ".." && ID_PATTERN.test(value);
}

/** 工程 ID 由调用方提供时同样校验。 */
export function assertValidId(value, what = "ID") {
    if (!isValidId(value)) {
        throw new Error(`${what} 不合法（需 1–128 位字母数字或 . _ -）：${value}`);
    }
    return value;
}
