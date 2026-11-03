// 组件演示页：仅用于 U1 视觉验收，不入正式页面导航。

import { confirmDialog, showDialog } from "./lib/dialog.js";
import { iconEl, loadIconManifest } from "./lib/icons.js";
import { initSplitters } from "./lib/splitter.js";
import { initTabs } from "./lib/tabs.js";
import { toast } from "./lib/toast.js";

const SWATCHES = [
    ["窗口底", "var(--bg-window)"],
    ["面板", "var(--bg-panel)"],
    ["深底", "var(--bg-deep)"],
    ["玻璃面", "var(--surface-strong)"],
    ["描边", "var(--stroke)"],
    ["主文字", "var(--text-1)"],
    ["次文字", "var(--text-2)"],
    ["弱文字", "var(--text-3)"],
    ["强调蓝", "var(--accent)"],
    ["品牌绿", "var(--brand)"],
    ["警告", "var(--warn)"],
    ["危险", "var(--danger)"],
    ["画布", "var(--canvas)"],
    ["网格", "var(--grid)"],
    ["连线", "var(--wire)"],
    ["终端", "var(--term-bg)"],
];

function renderSwatches() {
    const host = document.getElementById("swatches");
    for (const [name, value] of SWATCHES) {
        const card = document.createElement("div");
        card.className = "swatch";

        const fill = document.createElement("div");
        fill.className = "fill";
        fill.style.background = value;

        const meta = document.createElement("div");
        meta.className = "meta";
        const label = document.createElement("span");
        label.textContent = name;
        const code = document.createElement("code");
        code.textContent = value.replace("var(--", "").replace(")", "");
        meta.append(label, code);

        card.append(fill, meta);
        host.append(card);
    }
}

async function renderIcons() {
    const host = document.getElementById("icons");
    const manifest = await loadIconManifest();
    for (const [group, names] of Object.entries(manifest)) {
        const title = document.createElement("h3");
        title.textContent = `${group}（${names.length}）`;
        title.style.marginTop = "8px";
        const grid = document.createElement("div");
        grid.className = "icon-grid";
        for (const file of names) {
            const cell = document.createElement("div");
            cell.className = "icon-cell";
            const name = file.replace(/\.svg$/, "");
            cell.append(iconEl(group, name, { size: 20 }), document.createTextNode(name));
            grid.append(cell);
        }
        host.append(title, grid);
    }
}

initTabs();
initSplitters();
renderSwatches();
void renderIcons();

document.getElementById("btn-confirm").addEventListener("click", async () => {
    const ok = await confirmDialog({
        title: "保存工程",
        message: "当前文件已修改，是否保存后继续？",
        confirmText: "保存",
    });
    toast(ok ? "已保存" : "已取消", { type: ok ? "success" : "info" });
});

document.getElementById("btn-danger").addEventListener("click", async () => {
    const ok = await confirmDialog({
        title: "删除工程",
        message: "删除后不可恢复，确定删除？",
        confirmText: "删除",
        danger: true,
    });
    toast(ok ? "已删除" : "已取消", { type: ok ? "error" : "info" });
});

document.getElementById("btn-toast").addEventListener("click", () => {
    toast("这是一个提示");
});

document.addEventListener("tab-change", (event) => {
    console.log("tab:", event.detail.name);
});

// 显式引用 showDialog，避免未使用告警（演示页保留入口）
void showDialog;
