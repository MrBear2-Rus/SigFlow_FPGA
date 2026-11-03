// 工作区（U5）：六区外壳 + 真实面板（文件树 / 元件库 / 画布 / 编辑器 / 结构树 / 诊断）

import { api } from "./lib/api.js";
import { confirmDialog, showDialog } from "./lib/dialog.js";
import { iconEl } from "./lib/icons.js";
import { initSplitters } from "./lib/splitter.js";
import { toast } from "./lib/toast.js";
import { renderDiagnostics, renderStructure } from "./workspace/analysis.js";
import { CanvasEditor } from "./workspace/canvas-editor.js";
import { CodeEditor } from "./workspace/editor.js";
import { buildFileTree } from "./workspace/file-tree.js";

const $ = (id) => document.getElementById(id);
const REGIONS = ["left", "right", "bottom"];
const RAIL = [
    ["project", "folder", "工程"],
    ["tree", "tree", "结构树"],
    ["library", "components", "元件库"],
];
const LAYOUT_KEY = (user) => `sigflow.workspace.${user}`;

const ctx = {
    user: null,
    projectId: null,
    project: null,
    filePath: null,
    dirty: false,
    cursor: { line: 1, ch: 1 },
    files: [],
    structure: null,
    diagnostics: null,
    selectedElement: null,
    logs: [],
    canvas: null,
    editor: null,
    templates: [],
    gateIcons: {},
    agent: { sessionId: null, messages: [], busy: false },
    tabs: [],
    search: { open: false, query: "", replacement: "" },
    board: {
        loaded: false,
        targets: [],
        target: null,
        profile: null,
        pins: null,
        bindings: new Map(),
        diagnostics: null,
        filter: "",
        error: "",
    },
    log(text) {
        this.logs.push({ at: new Date(), text });
        if (this.logs.length > 200) this.logs.shift();
        if (state.layout?.bottom?.active === "terminal") renderRegion("bottom");
    },
};

const PANELS = {
    project: { label: "工程", icon: ["ui", "folder"], render: renderProjectPanel },
    tree: { label: "SigFlow 树", icon: ["ui", "tree"], render: renderTreePanel },
    library: { label: "元件库", icon: ["ui", "components"], render: renderLibraryPanel },
    property: { label: "属性", icon: ["ui", "file"], render: renderPropertyPanel },
    pins: { label: "引脚绑定", icon: ["signal", "module"], render: renderPinsPanel },
    terminal: { label: "终端", icon: ["ui", "start"], render: renderTerminalPanel },
    wave: { label: "波形", icon: ["ui", "file_v"], render: () => placeholder("ui", "file_v", "波形", "C++ P7 wave serve 之后接入") },
    assistant: { label: "助手", icon: ["signal", "icon"], render: renderAssistantPanel },
};

const DEFAULT_LAYOUT = {
    left: { tabs: ["project", "tree", "library"], active: "project", collapsed: false, size: 240 },
    right: { tabs: ["property", "pins", "assistant"], active: "property", collapsed: false, size: 260 },
    bottom: { tabs: ["terminal", "wave"], active: "terminal", collapsed: false, size: 180 },
};

const MENUS = [
    {
        id: "file",
        label: "File",
        items: [
            { label: "新建文件…", action: "newFile" },
            { label: "重命名 / 移动…", action: "renameFile" },
            { label: "删除文件…", action: "deleteFile", danger: true },
            { label: "打开工程…", href: "/pages/projects.html" },
            { label: "保存", action: "save", key: "Ctrl+S" },
            { separator: true },
            { label: "退出", href: "/" },
        ],
    },
    {
        id: "edit",
        label: "Edit",
        items: [
            { label: "撤销", disabled: true, key: "Ctrl+Z" },
            { label: "重做", disabled: true, key: "Ctrl+Y" },
            { label: "查找", disabled: true, key: "Ctrl+F" },
        ],
    },
    {
        id: "project",
        label: "Project",
        items: [
            { label: "工程列表", href: "/pages/projects.html" },
            { label: "解析当前文件", action: "structure" },
            { label: "语法检查", action: "lint" },
        ],
    },
    {
        id: "simulate",
        label: "Simulate",
        items: [
            { label: "运行仿真", disabled: true, key: "F6" },
            { label: "取消仿真", disabled: true, key: "Shift+F5" },
        ],
    },
    {
        id: "fpga",
        label: "FPGA",
        items: [
            { label: "FPGA 工具…", page: "fpga-tool" },
            { label: "引脚绑定…", disabled: true },
            { label: "TraceBridge 采集…", page: "tracebridge" },
            { label: "调试契约…", page: "debug-contract" },
        ],
    },
    {
        id: "help",
        label: "Help",
        items: [
            { label: "组件演示", href: "/pages/components.html" },
            { label: "关于", disabled: true },
        ],
    },
];

const TOOLS = [
    { icon: ["ui", "new"], title: "新建文件", action: "newFile" },
    { icon: ["ui", "open"], title: "打开工程", href: "/pages/projects.html" },
    { icon: ["ui", "save"], title: "保存文件", action: "save" },
    { separator: true },
    { icon: ["ui", "tree"], title: "解析", action: "structure" },
    { icon: ["ui", "file"], title: "语法检查", action: "lint" },
    { separator: true },
    { icon: ["ui", "start"], title: "开始", disabled: true },
    { icon: ["ui", "stop"], title: "停止", disabled: true },
    { separator: true },
    { label: "Synthesize", primary: true, disabled: true },
];

const ACTIONS = {
    save: () => saveFile(),
    newFile: () => newFile(),
    renameFile: () => renameFile(),
    deleteFile: () => deleteFile(),
    structure: () => runStructure(),
    lint: () => runLint(),
};

const state = {
    user: null,
    layout: null,
};

// ---- 启动 ------------------------------------------------------------------
async function boot() {
    try {
        const session = await api.get("/api/session");
        ctx.user = session.userId;
    } catch {
        location.href = "/";
        return;
    }

    const projectId = new URLSearchParams(location.search).get("project");
    if (!projectId) return fatal("未指定工程", "请从工程列表打开一个工程");
    try {
        ctx.project = await api.get(`/api/projects/${encodeURIComponent(projectId)}`);
        ctx.projectId = projectId;
    } catch {
        return fatal("工程不存在", "可能已被删除，或不属于当前用户");
    }

    state.user = ctx.user;
    state.layout = loadLayout(ctx.user);
    document.title = `${ctx.project.name ?? ctx.projectId} · SigFlow`;
    $("projectTitle").textContent = ctx.project.name ?? ctx.projectId;

    ctx.templates = await (await fetch("/assets/canvas_elements.json")).json();
    try {
        const icons = await (await fetch("/assets/component-icons.json")).json();
        ctx.gateIcons = icons.gateIcons ?? {};
    } catch {
        ctx.gateIcons = {};
    }

    renderMenus();
    renderTools();
    renderRail();
    ctx.canvas = new CanvasEditor($("canvasBoard"), {
        onChange: () => updateCanvasStatus(),
        onSelect: (element) => {
            ctx.selectedElement = element;
            if (state.layout.right.active === "property") renderRegion("right");
        },
        onLog: (text) => ctx.log(text),
        onView: (percent) => {
            const label = $("canvasZoom");
            if (label) label.textContent = `${percent}%`;
        },
    });
    ctx.canvas.setLibrary(ctx.templates);
    ctx.editor = new CodeEditor($("editorHost"), {
        onChange: (dirty) => {
            ctx.dirty = dirty;
            const tab = ctx.tabs.find((item) => item.path === ctx.filePath);
            if (tab) {
                tab.dirty = dirty;
                tab.content = ctx.editor.getValue();
            }
            updateEditorHead();
        },
        onCursor: (cursor) => {
            ctx.cursor = cursor;
            updateEditorHead();
        },
    });

    ctx.editor.cm.addKeyMap({
        "Ctrl-F": () => toggleSearch(),
        "Cmd-F": () => toggleSearch(),
        "Ctrl-S": () => void saveFile(),
        "Cmd-S": () => void saveFile(),
    });

    renderRegions();
    initSplitters();
    applyCollapsed();
    renderCenterToolbars();

    await Promise.all([refreshFiles(), loadCanvas()]);
    ctx.editor.refresh();
    ctx.log(`已打开工程 ${ctx.project.name ?? ctx.projectId}`);
    setStatus("就绪");
}

function fatal(title, message) {
    const host = document.createElement("div");
    host.className = "ws-fatal";
    const card = document.createElement("div");
    card.className = "card";
    card.append(iconEl("ui", "file_project", { size: 28 }));
    const heading = document.createElement("h2");
    heading.textContent = title;
    const text = document.createElement("p");
    text.className = "muted";
    text.textContent = message;
    const link = document.createElement("a");
    link.className = "btn btn-primary";
    link.href = "/pages/projects.html";
    link.textContent = "返回工程列表";
    card.append(heading, text, link);
    host.append(card);
    document.body.append(host);
}

// ---- 布局 ------------------------------------------------------------------
function loadLayout(user) {
    let saved = null;
    try {
        saved = JSON.parse(localStorage.getItem(LAYOUT_KEY(user)) ?? "null");
    } catch {
        saved = null;
    }
    const layout = structuredClone(DEFAULT_LAYOUT);
    if (saved && typeof saved === "object") {
        for (const region of REGIONS) {
            if (!saved[region]) continue;
            const tabs = Array.isArray(saved[region].tabs)
                ? saved[region].tabs.filter((id) => PANELS[id])
                : [];
            layout[region].tabs = tabs.length > 0 ? tabs : DEFAULT_LAYOUT[region].tabs;
            layout[region].active = PANELS[saved[region].active]
                ? saved[region].active
                : layout[region].tabs[0];
            layout[region].collapsed = Boolean(saved[region].collapsed);
            if (Number.isFinite(saved[region].size)) layout[region].size = saved[region].size;
        }
    }
    return layout;
}

function saveLayout() {
    try {
        localStorage.setItem(LAYOUT_KEY(state.user), JSON.stringify(state.layout));
    } catch {
        /* 隐私模式忽略 */
    }
}

// ---- 顶栏 ------------------------------------------------------------------
function renderMenus() {
    const host = $("menus");
    host.textContent = "";
    for (const menu of MENUS) {
        const wrap = document.createElement("div");
        wrap.className = "ws-menu";
        wrap.dataset.open = "false";

        const trigger = document.createElement("button");
        trigger.type = "button";
        trigger.textContent = menu.label;
        trigger.addEventListener("click", (event) => {
            event.stopPropagation();
            const open = wrap.dataset.open === "true";
            closeMenus();
            wrap.dataset.open = String(!open);
        });

        const list = document.createElement("div");
        list.className = "ws-menu-list";
        for (const item of menu.items) {
            if (item.separator) {
                const line = document.createElement("div");
                line.style.cssText = "height:1px;background:var(--stroke);margin:4px 6px";
                list.append(line);
                continue;
            }
            if (item.href || item.page) {
                const link = document.createElement("a");
                if (item.page) {
                    link.href = `/pages/${item.page}.html?project=${encodeURIComponent(ctx.projectId)}`;
                    link.target = "_blank";
                    link.rel = "noopener";
                } else {
                    link.href = item.href;
                }
                link.textContent = item.label;
                list.append(link);
                continue;
            }
            const button = document.createElement("button");
            button.type = "button";
            button.textContent = item.label;
            button.disabled = Boolean(item.disabled);
            if (item.key) {
                const kbd = document.createElement("kbd");
                kbd.textContent = item.key;
                button.append(kbd);
            }
            if (item.action) {
                button.addEventListener("click", () => {
                    closeMenus();
                    ACTIONS[item.action]?.();
                });
            }
            list.append(button);
        }
        wrap.append(trigger, list);
        host.append(wrap);
    }
}

function closeMenus() {
    for (const menu of document.querySelectorAll(".ws-menu")) menu.dataset.open = "false";
}

document.addEventListener("click", closeMenus);

function renderTools() {
    const host = $("tools");
    host.textContent = "";
    for (const tool of TOOLS) {
        if (tool.separator) {
            const sep = document.createElement("span");
            sep.className = "sep";
            host.append(sep);
            continue;
        }
        const node = tool.href ? document.createElement("a") : document.createElement("button");
        node.className = tool.label ? "btn btn-sm" : "btn btn-ghost btn-icon";
        if (tool.primary) node.classList.add("btn-primary");
        node.title = tool.title ?? tool.label ?? "";
        if (tool.href) node.href = tool.href;
        else {
            node.type = "button";
            node.disabled = Boolean(tool.disabled);
        }
        if (tool.icon) node.append(iconEl(tool.icon[0], tool.icon[1], { size: 15 }));
        if (tool.label) node.append(document.createTextNode(tool.label));
        if (tool.action) node.addEventListener("click", () => ACTIONS[tool.action]?.());
        host.append(node);
    }
}

// ---- 活动栏 ----------------------------------------------------------------
function renderRail() {
    const host = $("rail");
    host.textContent = "";
    for (const [panelId, icon, title] of RAIL) {
        const button = document.createElement("button");
        button.type = "button";
        button.title = title;
        button.setAttribute(
            "aria-pressed",
            String(state.layout.left.active === panelId && !state.layout.left.collapsed),
        );
        button.append(iconEl("ui", icon, { size: 18 }));
        button.addEventListener("click", () => {
            const left = state.layout.left;
            if (left.collapsed) {
                left.collapsed = false;
                regionEl("left").closest(".split").style.setProperty("--a", `${left.size ?? 240}px`);
            }
            if (!left.tabs.includes(panelId)) left.tabs.push(panelId);
            left.active = panelId;
            saveLayout();
            renderRegion("left");
            renderRail();
        });
        host.append(button);
    }
}

// ---- 区域 ------------------------------------------------------------------
function regionEl(name) {
    return document.querySelector(`[data-region="${name}"]`);
}

function renderRegions() {
    for (const name of REGIONS) renderRegion(name);
}

function renderRegion(name) {
    const el = regionEl(name);
    const head = el.querySelector(".ws-region-head");
    const body = el.querySelector(".ws-region-body");
    const region = state.layout[name];

    el.dataset.label = region.tabs.map((id) => PANELS[id].label).join(" / ");
    el.dataset.collapsed = String(region.collapsed);

    head.textContent = "";
    const tabs = document.createElement("div");
    tabs.className = "ws-region-tabs";

    for (const id of region.tabs) {
        const panel = PANELS[id];
        const tab = document.createElement("button");
        tab.type = "button";
        tab.className = "ws-tab";
        tab.draggable = true;
        tab.dataset.panel = id;
        tab.setAttribute("aria-selected", String(region.active === id));
        tab.append(iconEl(panel.icon[0], panel.icon[1], { size: 13 }), document.createTextNode(panel.label));
        tab.addEventListener("click", () => {
            region.active = id;
            saveLayout();
            renderRegion(name);
            if (name === "left") renderRail();
        });
        tab.addEventListener("dragstart", (event) => {
            event.dataTransfer.setData("text/plain", id);
            event.dataTransfer.effectAllowed = "move";
            tab.dataset.dragging = "true";
        });
        tab.addEventListener("dragend", () => {
            tab.dataset.dragging = "false";
        });
        tabs.append(tab);
    }

    const collapse = document.createElement("button");
    collapse.type = "button";
    collapse.className = "ws-collapse";
    collapse.textContent = region.collapsed ? "»" : "«";
    collapse.title = region.collapsed ? "展开" : "折叠";
    collapse.addEventListener("click", () => toggleCollapse(name));

    head.append(tabs, collapse);

    body.textContent = "";
    if (region.tabs.length === 0 || !region.active) {
        body.append(placeholder("ui", "open", "空区域", "把面板标签拖到这里"));
        return;
    }
    body.append(PANELS[region.active].render());

    el.ondragover = (event) => {
        event.preventDefault();
        event.dataTransfer.dropEffect = "move";
        el.dataset.drop = "true";
    };
    el.ondragleave = () => {
        el.dataset.drop = "false";
    };
    el.ondrop = (event) => {
        event.preventDefault();
        el.dataset.drop = "false";
        movePanel(event.dataTransfer.getData("text/plain"), name);
    };
}

function movePanel(panelId, targetRegion) {
    if (!PANELS[panelId]) return;
    for (const name of REGIONS) {
        const region = state.layout[name];
        const index = region.tabs.indexOf(panelId);
        if (index >= 0) region.tabs.splice(index, 1);
        if (region.active === panelId) region.active = region.tabs[0] ?? null;
    }
    const target = state.layout[targetRegion];
    target.tabs.push(panelId);
    target.active = panelId;
    saveLayout();
    for (const name of REGIONS) renderRegion(name);
    renderRail();
}

function toggleCollapse(name) {
    const region = state.layout[name];
    const el = regionEl(name);
    const container = el.closest(".split");
    const bottom = name === "bottom";
    if (!region.collapsed) {
        const current = Number.parseFloat(container.style.getPropertyValue("--a"));
        if (Number.isFinite(current) && current > 0) region.size = current;
        region.collapsed = true;
        container.style.setProperty("--a", "34px");
    } else {
        region.collapsed = false;
        container.style.setProperty("--a", `${region.size ?? (bottom ? 180 : 240)}px`);
    }
    saveLayout();
    renderRegion(name);
    renderRail();
}

function applyCollapsed() {
    for (const name of REGIONS) {
        if (!state.layout[name].collapsed) continue;
        regionEl(name).closest(".split").style.setProperty("--a", "34px");
    }
}

// ---- 中央 ------------------------------------------------------------------
function renderCenterToolbars() {
    const bar = $("canvasToolbar");
    bar.textContent = "";

    const wire = document.createElement("button");
    wire.type = "button";
    wire.className = "btn btn-sm";
    wire.textContent = "连线模式";
    wire.addEventListener("click", () => {
        const enabled = wire.getAttribute("aria-pressed") !== "true";
        wire.setAttribute("aria-pressed", String(enabled));
        wire.classList.toggle("btn-primary", enabled);
        ctx.canvas.setWireMode(enabled);
        updateCanvasStatus();
    });

    const remove = document.createElement("button");
    remove.type = "button";
    remove.className = "btn btn-sm";
    remove.textContent = "删除选中";
    remove.addEventListener("click", () => ctx.canvas.deleteSelected());

    const save = document.createElement("button");
    save.type = "button";
    save.className = "btn btn-sm";
    save.textContent = "保存画布";
    save.addEventListener("click", () => saveCanvas());

    const undo = document.createElement("button");
    undo.type = "button";
    undo.className = "btn btn-sm";
    undo.textContent = "撤销";
    undo.addEventListener("click", () => ctx.canvas.undo());
    const redo = document.createElement("button");
    redo.type = "button";
    redo.className = "btn btn-sm";
    redo.textContent = "重做";
    redo.addEventListener("click", () => ctx.canvas.redo());

    const zoomOut = document.createElement("button");
    zoomOut.type = "button";
    zoomOut.className = "btn btn-sm";
    zoomOut.textContent = "−";
    zoomOut.title = "缩小";
    zoomOut.addEventListener("click", () => ctx.canvas.zoomBy(1 / 1.2));
    const zoom = document.createElement("span");
    zoom.id = "canvasZoom";
    zoom.className = "muted small";
    zoom.textContent = `${ctx.canvas.zoomPercent}%`;
    const zoomIn = document.createElement("button");
    zoomIn.type = "button";
    zoomIn.className = "btn btn-sm";
    zoomIn.textContent = "+";
    zoomIn.title = "放大";
    zoomIn.addEventListener("click", () => ctx.canvas.zoomBy(1.2));
    const fit = document.createElement("button");
    fit.type = "button";
    fit.className = "btn btn-sm";
    fit.textContent = "适应";
    fit.addEventListener("click", () => ctx.canvas.resetView());

    const status = document.createElement("span");
    status.id = "canvasStatus";
    status.className = "muted small";
    status.style.marginLeft = "auto";

    bar.append(wire, remove, undo, redo, save, zoomOut, zoom, zoomIn, fit, status);
    updateCanvasStatus();

    const head = $("editorHead");
    head.textContent = "";
    const tabs = document.createElement("div");
    tabs.id = "editorTabs";
    tabs.className = "editor-tabs";
    const pos = document.createElement("span");
    pos.id = "editorPos";
    pos.className = "muted small";
    const dirty = document.createElement("span");
    dirty.id = "editorDirty";
    dirty.className = "dirty";
    const find = document.createElement("button");
    find.type = "button";
    find.className = "btn btn-sm";
    find.textContent = "查找";
    find.addEventListener("click", () => toggleSearch());
    const saveFileButton = document.createElement("button");
    saveFileButton.type = "button";
    saveFileButton.className = "btn btn-sm";
    saveFileButton.textContent = "保存";
    saveFileButton.addEventListener("click", () => saveFile());
    head.append(tabs, pos, dirty, find, saveFileButton);

    const search = document.createElement("div");
    search.id = "editorSearch";
    search.className = "editor-search";
    search.hidden = !ctx.search.open;
    const query = document.createElement("input");
    query.className = "input";
    query.placeholder = "查找";
    query.value = ctx.search.query;
    const replacement = document.createElement("input");
    replacement.className = "input";
    replacement.placeholder = "替换为";
    replacement.value = ctx.search.replacement;
    const prev = document.createElement("button");
    prev.type = "button";
    prev.className = "btn btn-sm";
    prev.textContent = "上一个";
    const next = document.createElement("button");
    next.type = "button";
    next.className = "btn btn-sm";
    next.textContent = "下一个";
    const replaceOne = document.createElement("button");
    replaceOne.type = "button";
    replaceOne.className = "btn btn-sm";
    replaceOne.textContent = "替换";
    const replaceAll = document.createElement("button");
    replaceAll.type = "button";
    replaceAll.className = "btn btn-sm";
    replaceAll.textContent = "全部替换";
    const close = document.createElement("button");
    close.type = "button";
    close.className = "btn btn-sm";
    close.textContent = "×";
    search.append(query, replacement, prev, next, replaceOne, replaceAll, close);
    head.after(search);

    const sync = () => {
        ctx.search.query = query.value;
        ctx.search.replacement = replacement.value;
    };
    query.addEventListener("input", sync);
    replacement.addEventListener("input", sync);
    query.addEventListener("keydown", (event) => {
        if (event.key === "Enter") {
            event.preventDefault();
            ctx.editor.find(ctx.search.query, { back: event.shiftKey });
        }
    });
    prev.addEventListener("click", () => ctx.editor.find(ctx.search.query, { back: true }));
    next.addEventListener("click", () => ctx.editor.find(ctx.search.query));
    replaceOne.addEventListener("click", () => {
        const ok = ctx.editor.replace(ctx.search.query, ctx.search.replacement);
        if (!ok) toast("没有匹配项", { type: "error" });
    });
    replaceAll.addEventListener("click", () => {
        const count = ctx.editor.replaceAll(ctx.search.query, ctx.search.replacement);
        toast(`替换 ${count} 处`);
        updateEditorHead();
    });
    close.addEventListener("click", () => toggleSearch(false));

    renderEditorTabs();
    updateEditorHead();
}

function renderEditorTabs() {
    const host = $("editorTabs");
    if (!host) return;
    host.textContent = "";
    for (const tab of ctx.tabs) {
        const item = document.createElement("div");
        item.className = "editor-tab";
        item.dataset.path = tab.path;
        item.title = tab.path;

        const name = document.createElement("span");
        name.textContent = tab.path.split("/").pop();
        const dot = document.createElement("span");
        dot.className = "tab-dirty";
        const close = document.createElement("button");
        close.type = "button";
        close.className = "tab-close";
        close.textContent = "×";
        close.title = "关闭";
        close.addEventListener("click", (event) => {
            event.stopPropagation();
            closeTab(tab.path);
        });

        item.append(name, dot, close);
        item.addEventListener("click", () => switchTab(tab.path));
        host.append(item);
    }
}

function toggleSearch(force) {
    ctx.search.open = force ?? !ctx.search.open;
    const bar = $("editorSearch");
    if (!bar) return;
    bar.hidden = !ctx.search.open;
    if (ctx.search.open) bar.querySelector("input")?.focus();
}

function updateCanvasStatus() {
    const status = $("canvasStatus");
    if (!status || !ctx.canvas) return;
    status.textContent =
        `元件 ${ctx.canvas.layout.elements.length} · 连线 ${ctx.canvas.layout.wires.length}`;
}

function updateEditorHead() {
    const pos = $("editorPos");
    if (!pos) return;
    pos.textContent = `${ctx.cursor.line}:${ctx.cursor.ch}`;
    const dirty = $("editorDirty");
    if (dirty) dirty.textContent = ctx.dirty ? "●" : "";
    for (const item of document.querySelectorAll(".editor-tab")) {
        const tab = ctx.tabs.find((entry) => entry.path === item.dataset.path);
        const dot = item.querySelector(".tab-dirty");
        if (dot) dot.textContent = tab?.dirty ? "●" : "";
        item.classList.toggle("active", item.dataset.path === ctx.filePath);
    }
    updateStatusRight();
}

function setStatus(text) {
    const label = $("statusText");
    if (label) label.textContent = text;
}

function updateStatusRight() {
    const right = $("statusRight");
    if (!right) return;
    right.textContent =
        `${ctx.filePath ?? "未打开文件"}  ${ctx.cursor.line}:${ctx.cursor.ch}` +
        (ctx.dirty ? "  ● 未保存" : "");
}

// ---- 文件 ------------------------------------------------------------------
async function refreshFiles() {
    const data = await api.get(`/api/projects/${encodeURIComponent(ctx.projectId)}/files`);
    ctx.files = data.files ?? [];
    if (state.layout.left.active === "project") renderRegion("left");
}

async function openFile(path) {
    if (ctx.tabs.some((tab) => tab.path === path)) {
        switchTab(path);
        return;
    }
    try {
        const data = await api.get(
            `/api/projects/${encodeURIComponent(ctx.projectId)}/file?path=${encodeURIComponent(path)}`,
        );
        ctx.tabs.push({ path, content: data.content ?? "", dirty: false });
        ctx.filePath = path;
        ctx.dirty = false;
        ctx.structure = null;
        ctx.diagnostics = null;
        ctx.editor.open(data.content ?? "");
        renderEditorTabs();
        updateEditorHead();
        ctx.log(`打开 ${path}`);
        setStatus(`已打开 ${path}`);
        if (state.layout.left.active === "tree") renderRegion("left");
        if (state.layout.right.active === "property") renderRegion("right");
    } catch (error) {
        toast(`打开失败：${error.message}`, { type: "error" });
    }
}

function switchTab(path) {
    if (ctx.filePath === path) return;
    const current = ctx.tabs.find((tab) => tab.path === ctx.filePath);
    if (current) {
        current.content = ctx.editor.getValue();
        current.dirty = ctx.dirty;
    }
    const tab = ctx.tabs.find((item) => item.path === path);
    if (!tab) return;
    ctx.filePath = path;
    ctx.dirty = tab.dirty;
    ctx.structure = null;
    ctx.diagnostics = null;
    ctx.editor.open(tab.content);
    ctx.editor.setDiagnostics([]);
    renderEditorTabs();
    updateEditorHead();
    ctx.log(`切换 ${path}`);
}

async function closeTab(path) {
    const tab = ctx.tabs.find((item) => item.path === path);
    if (!tab) return;
    if (tab.dirty) {
        const ok = await confirmDialog({
            title: "关闭未保存文件",
            message: `${path} 有未保存修改，关闭将丢弃这些修改。`,
            confirmText: "关闭",
            danger: true,
        });
        if (!ok) return;
    }
    removeTab(path);
}

function removeTab(path) {
    const index = ctx.tabs.findIndex((item) => item.path === path);
    if (index < 0) return;
    ctx.tabs.splice(index, 1);
    if (ctx.filePath === path) {
        const next = ctx.tabs[Math.max(0, index - 1)];
        ctx.filePath = next?.path ?? null;
        ctx.dirty = next?.dirty ?? false;
        ctx.structure = null;
        ctx.diagnostics = null;
        ctx.editor.open(next?.content ?? "");
        ctx.editor.setDiagnostics([]);
    }
    renderEditorTabs();
    updateEditorHead();
}

async function renameFile() {
    if (!ctx.filePath) return toast("请先打开文件", { type: "error" });
    const field = document.createElement("div");
    field.className = "field";
    const label = document.createElement("label");
    label.textContent = "新路径";
    const input = document.createElement("input");
    input.className = "input";
    input.value = ctx.filePath;
    field.append(label, input);

    await showDialog({
        title: "重命名 / 移动",
        content: field,
        actions: [
            { label: "取消", value: null },
            {
                label: "确定",
                primary: true,
                value: "ok",
                onClick: async () => {
                    const target = input.value.trim();
                    if (!target || target === ctx.filePath) return false;
                    try {
                        await api.post(
                            `/api/projects/${encodeURIComponent(ctx.projectId)}/file/move`,
                            { from: ctx.filePath, to: target },
                        );
                    } catch (error) {
                        toast(`重命名失败：${error.message}`, { type: "error" });
                        return false;
                    }
                    const tab = ctx.tabs.find((item) => item.path === ctx.filePath);
                    if (tab) tab.path = target;
                    ctx.filePath = target;
                    renderEditorTabs();
                    updateEditorHead();
                    await refreshFiles();
                    ctx.log(`重命名：${target}`);
                    toast("已重命名", { type: "success" });
                    return true;
                },
            },
        ],
    });
}

async function deleteFile() {
    if (!ctx.filePath) return toast("请先打开文件", { type: "error" });
    const ok = await confirmDialog({
        title: "删除文件",
        message: `${ctx.filePath} 将从工程中删除，且不可恢复。`,
        confirmText: "删除",
        danger: true,
    });
    if (!ok) return;
    try {
        await api.del(
            `/api/projects/${encodeURIComponent(ctx.projectId)}/file?path=${encodeURIComponent(ctx.filePath)}`,
        );
        ctx.log(`删除：${ctx.filePath}`);
        removeTab(ctx.filePath);
        await refreshFiles();
        toast("已删除", { type: "success" });
    } catch (error) {
        toast(`删除失败：${error.message}`, { type: "error" });
    }
}

async function saveFile() {
    if (!ctx.filePath) return toast("请先打开文件", { type: "error" });
    try {
        await api.put(
            `/api/projects/${encodeURIComponent(ctx.projectId)}/file?path=${encodeURIComponent(ctx.filePath)}`,
            { content: ctx.editor.getValue() },
        );
        ctx.dirty = false;
        const tab = ctx.tabs.find((item) => item.path === ctx.filePath);
        if (tab) {
            tab.content = ctx.editor.getValue();
            tab.dirty = false;
        }
        updateEditorHead();
        ctx.log(`保存 ${ctx.filePath}`);
        if (state.layout.right.active === "property") renderRegion("right");
        toast("已保存", { type: "success" });
        await refreshFiles();
    } catch (error) {
        toast(`保存失败：${error.message}`, { type: "error" });
    }
}

async function newFile() {
    const form = document.createElement("form");
    form.className = "project-form";
    form.addEventListener("submit", (event) => event.preventDefault());
    const field = document.createElement("div");
    field.className = "field";
    const label = document.createElement("label");
    label.textContent = "文件路径";
    const input = document.createElement("input");
    input.className = "input";
    input.placeholder = "例如 src/counter.v";
    const hint = document.createElement("span");
    hint.className = "form-hint";
    hint.textContent = "允许的扩展名：.v .sv .vh .svh .cst .json .md .txt .mem .hex .sdc";
    const error = document.createElement("p");
    error.className = "form-error";
    field.append(label, input, hint, error);
    form.append(field);

    let created = null;
    const result = await showDialog({
        title: "新建文件",
        content: form,
        actions: [
            { label: "取消", value: null },
            {
                label: "创建",
                value: "create",
                primary: true,
                onClick: async () => {
                    error.textContent = "";
                    const path = input.value.trim();
                    if (!/^[A-Za-z0-9._/-]{1,200}$/.test(path) || path.includes("..")) {
                        error.textContent = "路径不合法";
                        return false;
                    }
                    try {
                        await api.put(
                            `/api/projects/${encodeURIComponent(ctx.projectId)}/file?path=${encodeURIComponent(path)}`,
                            { content: "" },
                        );
                        created = path;
                        return true;
                    } catch (requestError) {
                        error.textContent = requestError.message;
                        return false;
                    }
                },
            },
        ],
    });
    if (result === "create" && created) {
        await refreshFiles();
        await openFile(created);
    }
}

// ---- 画布 ------------------------------------------------------------------
async function loadCanvas() {
    try {
        await ctx.canvas.load(ctx.projectId);
        updateCanvasStatus();
        ctx.log(`画布已加载：${ctx.canvas.layout.elements.length} 个元件`);
    } catch (error) {
        toast(`画布加载失败：${error.message}`, { type: "error" });
    }
}

async function saveCanvas() {
    try {
        await ctx.canvas.save(ctx.projectId);
        updateCanvasStatus();
        toast("画布已保存", { type: "success" });
    } catch (error) {
        toast(`画布保存失败：${error.message}`, { type: "error" });
    }
}

// ---- 分析 ------------------------------------------------------------------
async function runStructure() {
    if (!ctx.filePath) return toast("请先打开文件", { type: "error" });
    try {
        ctx.structure = await api.post(
            `/api/projects/${encodeURIComponent(ctx.projectId)}/structure`,
            { path: ctx.filePath },
        );
        ctx.log(`结构树：${ctx.structure.nodes?.length ?? 0} 个节点`);
        setStatus(`结构树：${ctx.structure.nodes?.length ?? 0} 个节点`);
        if (state.layout.left.active !== "tree") {
            state.layout.left.active = "tree";
            saveLayout();
        }
        renderRegion("left");
        renderRail();
    } catch (error) {
        toast(`解析失败：${error.message}`, { type: "error" });
    }
}

async function runLint() {
    if (!ctx.filePath) return toast("请先打开文件", { type: "error" });
    try {
        ctx.diagnostics = await api.post(
            `/api/projects/${encodeURIComponent(ctx.projectId)}/lint`,
            { path: ctx.filePath },
        );
        ctx.editor.setDiagnostics(ctx.diagnostics.diagnostics ?? []);
        ctx.log(`语法检查：${ctx.diagnostics.diagnostics?.length ?? 0} 条诊断`);
        setStatus(`语法检查：${ctx.diagnostics.diagnostics?.length ?? 0} 条诊断`);
        if (state.layout.left.active !== "tree") {
            state.layout.left.active = "tree";
            saveLayout();
        }
        renderRegion("left");
        renderRail();
    } catch (error) {
        toast(`检查失败：${error.message}`, { type: "error" });
    }
}

// ---- 面板 ------------------------------------------------------------------
function placeholder(group, icon, title, text) {
    const wrap = document.createElement("div");
    wrap.className = "empty";
    wrap.append(iconEl(group, icon, { size: 22 }));
    const strong = document.createElement("strong");
    strong.textContent = title;
    const span = document.createElement("span");
    span.className = "small";
    span.textContent = text;
    wrap.append(strong, span);
    return wrap;
}

function renderProjectPanel() {
    const wrap = document.createElement("div");
    wrap.className = "ws-project-card";

    const title = document.createElement("h3");
    title.textContent = ctx.project.name ?? ctx.projectId;

    const dl = document.createElement("dl");
    addRow(dl, "ID", ctx.projectId);
    addRow(dl, "模板", ctx.project.template === "sample" ? "示例" : "空白");
    addRow(dl, "更新", formatTime(ctx.project.updatedAt));

    const actions = document.createElement("div");
    actions.className = "analysis-head";
    const add = document.createElement("button");
    add.type = "button";
    add.className = "btn btn-sm";
    add.textContent = "新建文件";
    add.addEventListener("click", () => newFile());
    const rename = document.createElement("button");
    rename.type = "button";
    rename.className = "btn btn-sm";
    rename.textContent = "重命名";
    rename.addEventListener("click", () => renameFile());
    const remove = document.createElement("button");
    remove.type = "button";
    remove.className = "btn btn-sm";
    remove.textContent = "删除";
    remove.addEventListener("click", () => deleteFile());
    const refresh = document.createElement("button");
    refresh.type = "button";
    refresh.className = "btn btn-sm";
    refresh.textContent = "刷新";
    refresh.addEventListener("click", () =>
        refreshFiles().catch((error) => toast(error.message, { type: "error" })),
    );
    actions.append(add, rename, remove, refresh);

    const tree = document.createElement("div");
    tree.className = "file-tree-host";
    if (ctx.files.length === 0) {
        tree.append(placeholder("ui", "file", "暂无文件", "新建一个 .v 文件开始"));
    } else {
        tree.append(buildFileTree(ctx.files, (path) => openFile(path)));
    }

    wrap.append(title, dl, actions, tree);
    return wrap;
}

function renderTreePanel() {
    const wrap = document.createElement("div");
    wrap.className = "ws-project-card";

    const actions = document.createElement("div");
    actions.className = "analysis-head";
    const parse = document.createElement("button");
    parse.type = "button";
    parse.className = "btn btn-sm";
    parse.textContent = "解析";
    parse.disabled = !ctx.filePath;
    parse.addEventListener("click", () => runStructure());
    const lint = document.createElement("button");
    lint.type = "button";
    lint.className = "btn btn-sm";
    lint.textContent = "语法检查";
    lint.disabled = !ctx.filePath;
    lint.addEventListener("click", () => runLint());
    actions.append(parse, lint);
    wrap.append(actions);

    if (!ctx.filePath) {
        wrap.append(placeholder("ui", "tree", "未打开文件", "打开一个 .v 文件后可解析结构"));
        return wrap;
    }

    const structureHost = document.createElement("div");
    if (ctx.structure) {
        renderStructure(structureHost, ctx.structure.nodes ?? [], (line, col) => ctx.editor.goto(line, col));
    } else {
        structureHost.append(placeholder("ui", "tree", "尚未解析", "点击“解析”查看模块与信号"));
    }

    const diagTitle = document.createElement("h4");
    diagTitle.textContent = "诊断";
    const diagHost = document.createElement("div");
    if (ctx.diagnostics) {
        renderDiagnostics(diagHost, ctx.diagnostics.diagnostics ?? [], (line, col) => ctx.editor.goto(line, col));
    } else {
        diagHost.append(placeholder("ui", "file", "尚未检查", "点击“语法检查”运行 lint"));
    }

    wrap.append(structureHost, diagTitle, diagHost);
    return wrap;
}

function renderLibraryPanel() {
    const wrap = document.createElement("div");
    wrap.className = "ws-project-card";
    const grid = document.createElement("div");
    grid.className = "component-grid";

    for (const template of ctx.templates) {
        const icon = ctx.gateIcons[template.type];
        if (!icon) continue;
        const button = document.createElement("button");
        button.type = "button";
        button.className = "component-item";
        button.title = `放置 ${template.type} 门`;
        const img = document.createElement("img");
        img.src = `/assets/icons/components/${icon}`;
        img.alt = "";
        const label = document.createElement("span");
        label.textContent = template.type.toUpperCase();
        button.append(img, label);
        button.addEventListener("click", () => {
            ctx.canvas.addElement(template.type);
            updateCanvasStatus();
            ctx.log(`放置元件 ${template.type}`);
        });
        grid.append(button);
    }

    const hint = document.createElement("p");
    hint.className = "muted small";
    hint.style.marginTop = "8px";
    hint.textContent = "当前画布支持 6 种门电路；更多元件随画布模型扩展。";

    wrap.append(grid, hint);
    return wrap;
}

function renderPinsPanel() {
    const wrap = document.createElement("div");
    wrap.className = "ws-project-card pin-panel";

    if (!ctx.board.loaded) {
        wrap.append(placeholder("signal", "module", "引脚绑定", "选择目标板后编辑引脚分配"));
        const load = document.createElement("button");
        load.type = "button";
        load.className = "btn btn-primary btn-sm";
        load.textContent = "加载目标板";
        load.addEventListener("click", () => loadBoard(load));
        wrap.append(load);
        if (ctx.board.error) {
            const error = document.createElement("p");
            error.className = "form-error";
            error.textContent = ctx.board.error;
            wrap.append(error);
        }
        return wrap;
    }

    const head = document.createElement("div");
    head.className = "analysis-head";
    const select = document.createElement("select");
    select.className = "input";
    for (const target of ctx.board.targets) {
        const option = document.createElement("option");
        option.value = target.id;
        option.textContent = target.displayName ?? target.id;
        option.selected = target.id === ctx.board.target;
        select.append(option);
    }
    select.addEventListener("change", () => {
        ctx.board.target = select.value;
        ctx.board.loaded = false;
        loadBoard().catch((error) => toast(error.message, { type: "error" }));
    });

    const search = document.createElement("input");
    search.className = "input";
    search.placeholder = "筛选引脚 / 信号";
    search.value = ctx.board.filter;
    search.addEventListener("input", () => {
        ctx.board.filter = search.value.trim().toLowerCase();
        applyPinFilter(wrap);
    });

    const save = document.createElement("button");
    save.type = "button";
    save.className = "btn btn-sm";
    save.textContent = "保存 .cst";
    save.addEventListener("click", () => saveConstraints(save));
    const check = document.createElement("button");
    check.type = "button";
    check.className = "btn btn-primary btn-sm";
    check.textContent = "校验";
    check.addEventListener("click", () => validateConstraints(check));

    head.append(select, search, save, check);
    wrap.append(head);

    const table = document.createElement("table");
    table.className = "pin-table";
    const thead = document.createElement("thead");
    const headRow = document.createElement("tr");
    for (const label of ["Pin", "IO", "信号"]) {
        const th = document.createElement("th");
        th.textContent = label;
        headRow.append(th);
    }
    thead.append(headRow);
    const tbody = document.createElement("tbody");

    for (const pin of ctx.board.pins.pins) {
        const binding = ctx.board.bindings.get(pin.pin) ?? { signal: "", ioType: "" };
        const row = document.createElement("tr");
        if (!pin.available) row.className = "pin-reserved";

        const pinCell = document.createElement("td");
        pinCell.textContent = String(pin.pin);
        const ioCell = document.createElement("td");
        ioCell.textContent = pin.available ? pin.io : (pin.reserved ?? pin.io);

        const signalCell = document.createElement("td");
        const signal = document.createElement("input");
        signal.className = "input pin-input";
        signal.value = binding.signal;
        signal.placeholder = pin.available ? "未绑定" : "保留";
        signal.disabled = !pin.available;
        signal.addEventListener("input", () => {
            const current = ctx.board.bindings.get(pin.pin) ?? { signal: "", ioType: "" };
            current.signal = signal.value;
            ctx.board.bindings.set(pin.pin, current);
        });
        signalCell.append(signal);

        row.append(pinCell, ioCell, signalCell);
        tbody.append(row);
    }

    table.append(thead, tbody);
    const scroller = document.createElement("div");
    scroller.className = "pin-scroll";
    scroller.append(table);
    wrap.append(scroller);

    if (ctx.board.diagnostics) {
        const diagTitle = document.createElement("h4");
        diagTitle.textContent = "校验结果";
        const host = document.createElement("div");
        renderDiagnostics(
            host,
            (ctx.board.diagnostics.diagnostics ?? []).map((item) => ({
                severity: item.severity,
                code: item.content ? `${item.content}` : "",
                message: item.message,
                line: (item.line ?? 0) + 1,
                col: 1,
            })),
            () => {},
        );
        wrap.append(diagTitle, host);
    }

    applyPinFilter(wrap);
    return wrap;
}

function applyPinFilter(root) {
    const filter = ctx.board.filter;
    for (const row of root.querySelectorAll(".pin-table tbody tr")) {
        const text = row.textContent.toLowerCase() + " " + (row.querySelector(".pin-input")?.value ?? "").toLowerCase();
        row.style.display = !filter || text.includes(filter) ? "" : "none";
    }
}

async function loadBoard(button) {
    if (button) button.disabled = true;
    try {
        if (ctx.board.targets.length === 0) {
            const data = await api.get("/api/targets");
            ctx.board.targets = data.targets ?? [];
        }
        const target = ctx.board.target ?? ctx.board.targets[0]?.id;
        if (!target) throw new Error("没有可用的目标板数据");
        const board = await api.get(`/api/targets/${encodeURIComponent(target)}`);
        const constraints = await api.get(
            `/api/projects/${encodeURIComponent(ctx.projectId)}/constraints?target=${encodeURIComponent(target)}`,
        );
        ctx.board.target = target;
        ctx.board.profile = board.profile;
        ctx.board.pins = board.pins;
        ctx.board.bindings = parseCst(constraints.content ?? "");
        ctx.board.diagnostics = null;
        ctx.board.loaded = true;
        ctx.board.error = "";
        ctx.log(`加载目标板 ${target}：${board.pins.pins.length} 个引脚`);
        renderRegion("right");
    } catch (error) {
        ctx.board.error = error.message;
        toast(`加载目标板失败：${error.message}`, { type: "error" });
        renderRegion("right");
    } finally {
        if (button) button.disabled = false;
    }
}

function parseCst(content) {
    const bySignal = new Map();
    for (const raw of content.split(/\r?\n/)) {
        const line = raw.trim();
        const loc = /^IO_LOC\s+"([^"]+)"\s+(\d+)\s*;/.exec(line);
        if (loc) {
            bySignal.set(loc[1], { pin: Number(loc[2]), ioType: "" });
            continue;
        }
        const port = /^IO_PORT\s+"([^"]+)"\s+IO_TYPE=([A-Za-z0-9_]+)/.exec(line);
        if (port) {
            const binding = bySignal.get(port[1]) ?? { pin: null, ioType: "" };
            binding.ioType = port[2];
            bySignal.set(port[1], binding);
        }
    }
    const bindings = new Map();
    for (const [signal, info] of bySignal) {
        if (info.pin) bindings.set(info.pin, { signal, ioType: info.ioType });
    }
    return bindings;
}

function buildCst() {
    const fallback = ctx.board.pins?.default_io_type ?? ctx.board.profile?.default_io_type ?? "LVCMOS33";
    const lines = [];
    for (const [pin, binding] of [...ctx.board.bindings].sort((a, b) => a[0] - b[0])) {
        const signal = binding.signal.trim();
        if (!signal) continue;
        lines.push(`IO_LOC "${signal}" ${pin};`);
        lines.push(`IO_PORT "${signal}" IO_TYPE=${binding.ioType || fallback};`);
    }
    return lines.length > 0 ? `${lines.join("\n")}\n` : "";
}

async function saveConstraints(button) {
    button.disabled = true;
    try {
        await api.put(
            `/api/projects/${encodeURIComponent(ctx.projectId)}/constraints?target=${encodeURIComponent(ctx.board.target)}`,
            { content: buildCst() },
        );
        toast("约束已保存", { type: "success" });
        ctx.log(`保存约束：${ctx.board.target}`);
        await refreshFiles();
    } catch (error) {
        toast(`保存约束失败：${error.message}`, { type: "error" });
    } finally {
        button.disabled = false;
    }
}

async function validateConstraints(button) {
    button.disabled = true;
    try {
        const result = await api.post(
            `/api/projects/${encodeURIComponent(ctx.projectId)}/validate-cst`,
            { target: ctx.board.target },
        );
        ctx.board.diagnostics = result;
        ctx.log(`约束校验：${result.valid ? "通过" : "未通过"}，${result.diagnostics?.length ?? 0} 条诊断`);
        renderRegion("right");
    } catch (error) {
        toast(`校验失败：${error.message}`, { type: "error" });
    } finally {
        button.disabled = false;
    }
}

function renderPropertyPanel() {
    const wrap = document.createElement("div");
    wrap.className = "ws-project-card";

    if (ctx.selectedElement) {
        const element = ctx.selectedElement;
        const title = document.createElement("h3");
        title.textContent = element.label ?? element.type;
        const dl = document.createElement("dl");
        addRow(dl, "类型", element.type);

        const labelField = document.createElement("input");
        labelField.className = "input";
        labelField.value = element.label ?? "";
        labelField.addEventListener("change", () => {
            ctx.canvas.updateElement(element.id, { label: labelField.value.trim() || element.type });
        });
        const labelWrap = document.createElement("div");
        labelWrap.className = "field";
        const labelCaption = document.createElement("label");
        labelCaption.textContent = "标签";
        labelWrap.append(labelCaption, labelField);

        const xField = document.createElement("input");
        xField.type = "number";
        xField.className = "input";
        xField.value = String(Math.round(element.x));
        xField.addEventListener("change", () => {
            ctx.canvas.updateElement(element.id, { x: Number(xField.value) });
        });
        const yField = document.createElement("input");
        yField.type = "number";
        yField.className = "input";
        yField.value = String(Math.round(element.y));
        yField.addEventListener("change", () => {
            ctx.canvas.updateElement(element.id, { y: Number(yField.value) });
        });
        const posRow = document.createElement("div");
        posRow.className = "field-row";
        const xLabel = document.createElement("label");
        xLabel.textContent = "X";
        const yLabel = document.createElement("label");
        yLabel.textContent = "Y";
        posRow.append(xLabel, xField, yLabel, yField);

        wrap.append(title, dl, labelWrap, posRow);
        return wrap;
    }

    if (ctx.filePath) {
        const title = document.createElement("h3");
        title.textContent = ctx.filePath;
        const dl = document.createElement("dl");
        addRow(dl, "行数", String(ctx.editor.lineCount));
        addRow(dl, "光标", `${ctx.cursor.line}:${ctx.cursor.ch}`);
        addRow(dl, "状态", ctx.dirty ? "未保存" : "已保存");
        wrap.append(title, dl);
        return wrap;
    }

    wrap.append(placeholder("ui", "file", "属性", "选中画布元件或打开文件后显示"));
    return wrap;
}

function renderAssistantPanel() {
    const wrap = document.createElement("div");
    wrap.className = "ws-project-card agent-panel";

    if (!ctx.agent.sessionId) {
        wrap.append(placeholder("signal", "icon", "教学助手", "为当前工程建立一个教学会话"));
        const start = document.createElement("button");
        start.type = "button";
        start.className = "btn btn-primary btn-sm";
        start.textContent = "新建会话";
        start.addEventListener("click", () => startAgentSession(start));
        wrap.append(start);
        return wrap;
    }

    const log = document.createElement("div");
    log.className = "agent-messages";
    if (ctx.agent.messages.length === 0) {
        log.append(Object.assign(document.createElement("p"), {
            className: "muted small",
            textContent: "会话已建立。打开一个文件后，可以让助手解释当前代码。",
        }));
    }
    for (const message of ctx.agent.messages) {
        const item = document.createElement("div");
        item.className = `agent-msg agent-${message.role}`;
        item.textContent = message.text;
        log.append(item);
    }

    const actions = document.createElement("div");
    actions.className = "agent-form";
    const explain = document.createElement("button");
    explain.type = "button";
    explain.className = "btn btn-primary btn-sm";
    explain.textContent = "解释当前文件";
    explain.disabled = !ctx.filePath || ctx.agent.busy;
    explain.addEventListener("click", () => sendAgentExplain());
    const reset = document.createElement("button");
    reset.type = "button";
    reset.className = "btn btn-sm";
    reset.textContent = "结束会话";
    reset.addEventListener("click", () => {
        ctx.agent = { sessionId: null, messages: [], busy: false };
        renderRegion("right");
    });
    actions.append(explain, reset);

    wrap.append(log, actions);
    return wrap;
}

async function startAgentSession(button) {
    button.disabled = true;
    try {
        const response = await api.post("/api/agent/sessions", {
            project_id: ctx.projectId,
            revision: ctx.project.updatedAt ?? "1",
            top: "top",
        });
        ctx.agent.sessionId = response.data?.session_id ?? response.session_id;
        ctx.agent.messages = [];
        ctx.log(`Agent 会话：${ctx.agent.sessionId}`);
    } catch (error) {
        toast(`Agent 会话失败：${error.message}`, { type: "error" });
    } finally {
        button.disabled = false;
        renderRegion("right");
    }
}

async function sendAgentExplain() {
    if (!ctx.filePath || ctx.agent.busy) return;
    ctx.agent.busy = true;
    const selectionText = ctx.editor.getValue().slice(0, 60_000);
    ctx.agent.messages.push({ role: "user", text: `解释 ${ctx.filePath}` });
    renderRegion("right");
    try {
        const response = await api.post(
            `/api/agent/sessions/${encodeURIComponent(ctx.agent.sessionId)}/runs`,
            {
                kind: "explain",
                level: "L1",
                selection: { text: selectionText, start_line: 1, end_line: ctx.editor.lineCount },
            },
        );
        const cards = response.data?.cards ?? [];
        const answer = cards
            .map((card) => [card.title, card.body].filter(Boolean).join("\n"))
            .filter(Boolean)
            .join("\n\n");
        ctx.agent.messages.push({ role: "agent", text: answer || "（没有返回内容）" });
    } catch (error) {
        ctx.agent.messages.push({ role: "error", text: `请求失败：${error.message}` });
    } finally {
        ctx.agent.busy = false;
        renderRegion("right");
    }
}

function renderTerminalPanel() {
    const wrap = document.createElement("div");
    wrap.className = "terminal-log";
    if (ctx.logs.length === 0) {
        wrap.append(placeholder("ui", "start", "终端", "操作日志会显示在这里"));
        return wrap;
    }
    for (const entry of ctx.logs) {
        const line = document.createElement("div");
        const time = document.createElement("span");
        time.className = "time";
        time.textContent = entry.at.toLocaleTimeString("zh-CN", { hour12: false });
        line.append(time, document.createTextNode(entry.text));
        wrap.append(line);
    }
    return wrap;
}

function addRow(dl, label, value) {
    const dt = document.createElement("dt");
    dt.textContent = label;
    const dd = document.createElement("dd");
    dd.textContent = value ?? "—";
    dl.append(dt, dd);
}

function formatTime(iso) {
    if (!iso) return "—";
    const date = new Date(iso);
    if (Number.isNaN(date.getTime())) return "—";
    const pad = (value) => String(value).padStart(2, "0");
    return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())} ` +
        `${pad(date.getHours())}:${pad(date.getMinutes())}`;
}

// ---- 快捷键 ----------------------------------------------------------------
window.addEventListener("keydown", (event) => {
    const mod = event.ctrlKey || event.metaKey;
    if (mod && event.key.toLowerCase() === "s") {
        event.preventDefault();
        void saveFile();
    }
});

// ---- 重置布局 --------------------------------------------------------------
$("resetLayout").addEventListener("click", () => {
    try {
        localStorage.removeItem(LAYOUT_KEY(state.user));
        for (const key of Object.keys(localStorage)) {
            if (key.startsWith("sigflow.split.ws-")) localStorage.removeItem(key);
        }
    } catch {
        /* 忽略 */
    }
    toast("布局已恢复默认");
    setTimeout(() => location.reload(), 200);
});

void boot();
