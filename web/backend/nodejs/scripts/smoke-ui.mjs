#!/usr/bin/env node
// 浏览器冒烟：起一个临时 Chromium，走登录 → 工程 → 工作区 → 独立窗口。
// 用法：node backend/nodejs/scripts/smoke-ui.mjs [http://127.0.0.1:8080]
// 依赖：本机 chromium（可用 CHROME_BIN 覆盖）与已启动的 sigflow-web。

import { spawn } from "node:child_process";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

const BASE = (process.argv[2] ?? "http://127.0.0.1:8080").replace(/\/$/, "");
const CHROME = process.env.CHROME_BIN ?? "chromium";
const PORT = Number(process.env.SMOKE_DEBUG_PORT ?? 9333);

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const results = [];

function check(name, ok, detail = "") {
    results.push({ name, ok: Boolean(ok), detail });
}

async function waitFor(fn, timeoutMs = 15000) {
    const started = Date.now();
    while (Date.now() - started < timeoutMs) {
        try {
            if (await fn()) return true;
        } catch {
            /* 继续等 */
        }
        await sleep(200);
    }
    return false;
}

class Session {
    constructor(socket) {
        this.socket = socket;
        this.pending = new Map();
        this.events = new Map();
        this.errors = [];
        this.sequence = 0;
        socket.onmessage = (event) => {
            const message = JSON.parse(event.data);
            if (message.id && this.pending.has(message.id)) {
                const { resolve, reject } = this.pending.get(message.id);
                this.pending.delete(message.id);
                message.error ? reject(new Error(message.error.message)) : resolve(message.result);
                return;
            }
            if (message.method === "Runtime.exceptionThrown") {
                const details = message.params.exceptionDetails;
                this.errors.push(details.exception?.description ?? details.text ?? "exception");
            }
            if (message.method === "Runtime.consoleAPICalled" && message.params.type === "error") {
                this.errors.push(
                    message.params.args.map((arg) => arg.value ?? arg.description ?? "").join(" "),
                );
            }
            const listeners = this.events.get(message.method);
            if (listeners?.length) listeners.shift()(message.params);
        };
    }

    send(method, params = {}) {
        const id = ++this.sequence;
        return new Promise((resolve, reject) => {
            this.pending.set(id, { resolve, reject });
            this.socket.send(JSON.stringify({ id, method, params }));
        });
    }

    once(method, timeoutMs = 10000) {
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => reject(new Error(`等待 ${method} 超时`)), timeoutMs);
            const list = this.events.get(method) ?? [];
            list.push((params) => {
                clearTimeout(timer);
                resolve(params);
            });
            this.events.set(method, list);
        });
    }

    async goto(url) {
        const loaded = this.once("Page.loadEventFired");
        await this.send("Page.navigate", { url });
        await loaded;
        await sleep(300);
    }

    async eval(expression) {
        const result = await this.send("Runtime.evaluate", {
            expression,
            awaitPromise: true,
            returnByValue: true,
        });
        if (result.exceptionDetails) {
            throw new Error(
                result.exceptionDetails.exception?.description ?? result.exceptionDetails.text,
            );
        }
        return result.result?.value;
    }
}

async function main() {
    const profile = await mkdtemp(join(tmpdir(), "sigflow-smoke-"));
    const chrome = spawn(
        CHROME,
        [
            "--headless=new",
            "--no-sandbox",
            "--disable-gpu",
            `--remote-debugging-port=${PORT}`,
            `--user-data-dir=${profile}`,
            "about:blank",
        ],
        { stdio: ["ignore", "ignore", "pipe"] },
    );
    let chromeError = "";
    chrome.stderr.on("data", (chunk) => {
        chromeError += chunk;
    });

    let session = null;
    try {
        const ready = await waitFor(async () => {
            const response = await fetch(`http://127.0.0.1:${PORT}/json/version`);
            return response.ok;
        });
        if (!ready) throw new Error(`Chromium 未启动：${chromeError.slice(-400)}`);

        const targets = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json();
        const page = targets.find((target) => target.type === "page");
        const socket = new WebSocket(page.webSocketDebuggerUrl);
        await new Promise((resolve) => (socket.onopen = resolve));
        session = new Session(socket);

        await session.send("Page.enable");
        await session.send("Runtime.enable");
        await session.send("Emulation.setDeviceMetricsOverride", {
            width: 1440,
            height: 900,
            deviceScaleFactor: 1,
            mobile: false,
        });

        const userId = `smoke_${Math.random().toString(36).slice(2, 8)}`;
        await session.goto(`${BASE}/`);
        const landing = await session.eval("document.querySelector('h1')?.textContent ?? ''");
        check("落地页渲染", landing.trim().length > 0, landing.trim());

        const registered = await session.eval(`(async () => {
            const response = await fetch("/api/users", {
                method: "POST",
                headers: { "content-type": "application/json" },
                body: JSON.stringify({ userId: "${userId}", password: "password123" }),
            });
            return response.status;
        })()`);
        check("注册", registered === 201, `HTTP ${registered}`);

        const loggedIn = await session.eval(`(async () => {
            const response = await fetch("/api/session", {
                method: "POST",
                headers: { "content-type": "application/json" },
                body: JSON.stringify({ userId: "${userId}", password: "password123" }),
            });
            return response.status;
        })()`);
        check("登录", loggedIn === 200, `HTTP ${loggedIn}`);

        const created = await session.eval(`(async () => {
            const project = await fetch("/api/projects", {
                method: "POST",
                headers: { "content-type": "application/json" },
                body: JSON.stringify({ projectId: "smoke", name: "冒烟工程", template: "blank" }),
            });
            const file = await fetch("/api/projects/smoke/file?path=src%2Ftop.v", {
                method: "PUT",
                headers: { "content-type": "application/json" },
                body: JSON.stringify({ content: "module top(input clk, output reg led);\\nendmodule\\n" }),
            });
            return project.status + "/" + file.status;
        })()`);
        check("建工程与文件", created === "201/200", created);

        await session.goto(`${BASE}/pages/projects.html`);
        const projectText = await session.eval("document.body.innerText");
        check("工程列表显示", projectText.includes("smoke"));

        await session.goto(`${BASE}/pages/workspace.html?project=smoke`);
        const tabs = await session.eval("document.querySelectorAll('.ws-tab').length");
        check("工作区六区页签", tabs >= 3, String(tabs));
        await session.eval(`[...document.querySelectorAll(".file-item")].find(e => e.textContent.includes("top.v"))?.click()`);
        await sleep(600);
        const editorText = await session.eval("document.querySelector('.CodeMirror')?.CodeMirror.getValue() ?? ''");
        check("编辑器打开文件", editorText.includes("module top"), editorText.slice(0, 30));

        await session.eval(`[...document.querySelectorAll(".ws-tab")].find(t => t.textContent.includes("SigFlow"))?.click()`);
        await sleep(300);
        await session.eval(`[...document.querySelectorAll("[data-region=left] button")].find(b => b.textContent === "解析")?.click()`);
        await sleep(900);
        const treeCount = await session.eval("document.querySelectorAll('[data-region=left] .structure-row').length");
        check("结构树解析", treeCount > 0, String(treeCount));

        await session.goto(`${BASE}/pages/fpga-tool.html?project=smoke`);
        const fpgaTabs = await session.eval("[...document.querySelectorAll('.tab')].map(t => t.textContent).join('|')");
        check("FPGA 工具页签", fpgaTabs.includes("仿真"), fpgaTabs);

        await session.goto(`${BASE}/pages/tracebridge.html?project=smoke`);
        check("TraceBridge 打开", (await session.eval("document.body.innerText")).length > 0);
        await session.goto(`${BASE}/pages/debug-contract.html?project=smoke`);
        check("契约配置打开", (await session.eval("document.body.innerText")).length > 0);

        await session.eval(`(async () => {
            await fetch("/api/projects/smoke", { method: "DELETE" });
            return 1;
        })()`);
    } finally {
        if (session) {
            check("无 JS 异常", session.errors.length === 0, session.errors.slice(0, 3).join(" | "));
        }
        chrome.kill("SIGTERM");
        await sleep(300);
        chrome.kill("SIGKILL");
        await rm(profile, { recursive: true, force: true });
    }

    let failed = 0;
    for (const item of results) {
        console.log(`${item.ok ? "✔" : "✖"} ${item.name}${item.detail ? ` — ${item.detail}` : ""}`);
        if (!item.ok) failed += 1;
    }
    console.log(`\n${results.length - failed}/${results.length} 通过`);
    if (failed > 0) process.exitCode = 1;
}

main().catch((error) => {
    console.error(`冒烟失败：${error.message}`);
    process.exitCode = 1;
});
