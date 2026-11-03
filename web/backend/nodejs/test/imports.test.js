// 前端引用检查：HTML 的静态路径与 JS 的 import 图必须都能落到真实文件
// （http 测试只请求 URL，这里补的是 JS 模块图与本地相对路径）

import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import { readdir, readFile } from "node:fs/promises";
import { dirname, join, normalize, resolve } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const FRONTEND = fileURLToPath(new URL("../../../frontend/", import.meta.url));

function urlToFile(url) {
    if (url === "/") return join(FRONTEND, "pages", "index.html");
    const clean = url.split("?")[0].split("#")[0];
    return join(FRONTEND, clean.replace(/^\//, ""));
}

async function walk(dir, out = []) {
    for (const entry of await readdir(dir, { withFileTypes: true })) {
        const path = join(dir, entry.name);
        if (entry.isDirectory()) await walk(path, out);
        else if (entry.name.endsWith(".js")) out.push(path);
    }
    return out;
}

test("HTML 的 src/href 指向真实文件", async () => {
    const pagesDir = join(FRONTEND, "pages");
    let checked = 0;
    for (const file of await readdir(pagesDir)) {
        if (!file.endsWith(".html")) continue;
        const html = await readFile(join(pagesDir, file), "utf8");
        const refs = [...html.matchAll(/(?:src|href)="(\/[^"]*)"/g)].map((match) => match[1]);
        for (const ref of refs) {
            const target = urlToFile(ref);
            assert.ok(existsSync(target), `${file} 引用了不存在的文件：${ref}`);
            checked += 1;
        }
    }
    assert.ok(checked > 20, `检查的引用过少：${checked}`);
});

test("JS 的 import 图没有断链", async () => {
    const files = await walk(join(FRONTEND, "scripts"));
    let checked = 0;
    for (const file of files) {
        const source = await readFile(file, "utf8");
        const imports = [
            ...source.matchAll(/(?:from|import)\s*\(?\s*["'](\.{1,2}\/[^"']+)["']/g),
        ].map((match) => match[1]);
        for (const specifier of imports) {
            const target = normalize(resolve(dirname(file), specifier));
            assert.ok(
                existsSync(target),
                `${file.replace(FRONTEND, "")} 引用不存在：${specifier}`,
            );
            checked += 1;
        }
    }
    assert.ok(checked > 20, `检查的 import 过少：${checked}`);
});
