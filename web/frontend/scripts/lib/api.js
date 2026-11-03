// =============================================================================
// 统一请求封装：同源 Cookie、JSON、错误码透传
// =============================================================================

export class ApiError extends Error {
    constructor(message, { status = 0, code = "" } = {}) {
        super(message);
        this.name = "ApiError";
        this.status = status;
        this.code = code;
    }
}

/**
 * @param {string} path
 * @param {{method?: string, body?: unknown, headers?: object, signal?: AbortSignal}} [options]
 */
export async function request(path, options = {}) {
    const { method = "GET", body, headers = {}, signal } = options;
    const init = { method, credentials: "same-origin", headers: { ...headers }, signal };
    if (body !== undefined) {
        init.body = typeof body === "string" ? body : JSON.stringify(body);
        init.headers["content-type"] ??= "application/json";
    }

    let response;
    try {
        response = await fetch(path, init);
    } catch {
        throw new ApiError("无法连接服务器", { status: 0, code: "NETWORK" });
    }
    if (response.status === 204) return null;

    if (response.status === 401 && !path.startsWith("/api/session")) {
        const page = location.pathname;
        if (page !== "/" && !page.endsWith("/index.html")) location.assign("/");
    }

    const text = await response.text();
    let data = null;
    if (text) {
        try {
            data = JSON.parse(text);
        } catch {
            data = text;
        }
    }

    if (!response.ok) {
        const message = data?.error?.message ?? `HTTP ${response.status}`;
        throw new ApiError(message, { status: response.status, code: data?.error?.code ?? "" });
    }
    return data;
}

export const api = {
    get: (path, options) => request(path, options),
    post: (path, body, options) => request(path, { ...options, method: "POST", body }),
    put: (path, body, options) => request(path, { ...options, method: "PUT", body }),
    del: (path, options) => request(path, { ...options, method: "DELETE" }),
};
