// 文档一致性检查：头部格式、阶段同步、行数上限、页面引用、已删除文件

import assert from "node:assert/strict";
import { readFile, readdir } from "node:fs/promises";
import { join } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const WEB = fileURLToPath(new URL("../../../", import.meta.url));
const DOCS = join(WEB, "current_status_docs");

const FILES = {
    plan: "plan-js.md",
    handoff: "handoff-js.md",
    history: "history-js.md",
};
const TYPES = { plan: "规划", handoff: "交接", history: "实况" };
const LIMITS = { plan: 120, handoff: 80, history: 150 };

async function load(name) {
    return readFile(join(DOCS, name), "utf8");
}

function header(text) {
    const fields = {};
    for (const line of text.split("\n")) {
        const match = /^> (负责人|类型|更新|状态|阶段)：(.+)$/.exec(line);
        if (match) fields[match[1]] = match[2].trim();
    }
    return fields;
}

function section(text, title) {
    const lines = text.split("\n");
    const start = lines.findIndex((line) => line.startsWith(`## ${title}`));
    if (start < 0) return "";
    const rest = lines.slice(start + 1);
    const end = rest.findIndex((line) => line.startsWith("## "));
    return (end < 0 ? rest : rest.slice(0, end)).join("\n");
}

test("status 文档：头部字段、类型与阶段一致", async () => {
    const texts = {};
    const headers = {};
    for (const [kind, file] of Object.entries(FILES)) {
        texts[kind] = await load(file);
        headers[kind] = header(texts[kind]);
        for (const key of ["负责人", "类型", "更新", "状态", "阶段"]) {
            assert.ok(headers[kind][key], `${file} 缺少头部字段：${key}`);
        }
        assert.equal(headers[kind]["类型"], TYPES[kind], `${file} 类型错误`);
    }
    const stages = new Set(Object.values(headers).map((item) => item["阶段"]));
    assert.equal(stages.size, 1, `三份文档阶段不一致：${[...stages].join(" / ")}`);
});

test("status 文档：行数与条目上限", async () => {
    for (const [kind, file] of Object.entries(FILES)) {
        const text = await load(file);
        const lines = text.trimEnd().split("\n").length;
        assert.ok(lines <= LIMITS[kind], `${file} 超过 ${LIMITS[kind]} 行（当前 ${lines}）`);
    }

    const plan = await load(FILES.plan);
    const steps = section(plan, "⑤ 下一步").split("\n").filter((line) => /^\d+\./.test(line));
    assert.ok(steps.length <= 5, `plan 的「下一步」超过 5 条（当前 ${steps.length}）`);

    const handoff = await load(FILES.handoff);
    const changes = section(handoff, "④ 最近变更").split("\n").filter((line) => /^- /.test(line));
    assert.ok(changes.length <= 10, `handoff 的「最近变更」超过 10 条（当前 ${changes.length}）`);
});

test("文档不引用已删除的页面与样式", async () => {
    const removed = ["console.html", "canvas.html", "landing.html", "app.css"];
    const files = [
        ...Object.values(FILES).map((name) => join(DOCS, name)),
        join(WEB, "README.md"),
    ];
    for (const file of files) {
        const text = await readFile(file, "utf8");
        for (const name of removed) {
            assert.ok(!text.includes(name), `${file} 仍引用已删除文件：${name}`);
        }
    }
});

test("每个前端页面都在文档中被提及", async () => {
    const text = [await load(FILES.plan), await readFile(join(WEB, "README.md"), "utf8")].join("\n");
    const pages = (await readdir(join(WEB, "frontend", "pages"))).filter((name) => name.endsWith(".html"));
    assert.ok(pages.length > 0, "pages 目录为空");
    for (const page of pages) {
        assert.ok(text.includes(page), `页面未在文档中登记：${page}`);
    }
});
