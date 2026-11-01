// =============================================================================
// SigFlow 控制台（J4）
//
// 账号 → 工程 → 文件 → 编辑器（CodeMirror）→ 结构树/诊断 → 任务。
// 编辑器与 CodeMirror 均为本地 vendor，不联网。
// =============================================================================

const $ = (id) => document.getElementById(id);

const DEFAULT_PARAMS = {
    top_module: "top",
    source_files: ["src/top.v"],
    strategy: "baseline",
    yosys_family: "gw1n",
};

const state = {
    user: null,
    projects: [],
    project: null,
    jobId: null,
    source: null,
    editor: null,
};

// ---- 通用请求 --------------------------------------------------------------
async function api(path, options = {}) {
    const headers = { ...(options.headers ?? {}) };
    if (options.body !== undefined && typeof options.body === "string" && !headers["content-type"]) {
        headers["content-type"] = "application/json";
    }
    const response = await fetch(path, { credentials: "same-origin", ...options, headers });
    if (response.status === 204) return null;
    const data = await response.json().catch(() => null);
    if (!response.ok) {
        const error = new Error(data?.error?.message ?? `HTTP ${response.status}`);
        error.code = data?.error?.code;
        error.status = response.status;
        throw error;
    }
    return data;
}

function setStatus(text) {
    $("status").textContent = text;
}

function appendLog(line) {
    const log = $("log");
    log.textContent += `${line}\n`;
    log.scrollTop = log.scrollHeight;
}

// ---- 编辑器 ----------------------------------------------------------------
function initEditor() {
    state.editor = CodeMirror.fromTextArea($("fileContent"), {
        mode: "text/x-verilog",
        lineNumbers: true,
        matchBrackets: true,
        indentUnit: 4,
        tabSize: 4,
        lineWrapping: false,
    });
    state.editor.setSize("100%", "46vh");
}

// ---- 健康状态 --------------------------------------------------------------
async function refreshHealth() {
    try {
        const data = await api("/api/health");
        $("health").textContent =
            `服务正常 · 运行中 ${data.jobs.running} · 排队 ${data.jobs.queued}`;
    } catch {
        $("health").textContent = "服务不可用";
    }
}

// ---- 账号 ------------------------------------------------------------------
function showAuthed(userId) {
    state.user = userId;
    $("account").textContent = `已登录：${userId}`;
    $("authStatus").textContent = "";
    $("login").hidden = true;
    $("register").hidden = true;
    $("logout").hidden = false;
    $("userId").disabled = true;
    $("password").disabled = true;
    $("projectPanel").hidden = false;
    $("editorPanel").hidden = false;
    $("jobPanel").hidden = false;
    state.editor.refresh();
    void loadProjects();
}

function showAnonymous() {
    state.user = null;
    state.project = null;
    $("account").textContent = "未登录";
    $("login").hidden = false;
    $("register").hidden = false;
    $("logout").hidden = true;
    $("userId").disabled = false;
    $("password").disabled = false;
    $("projectPanel").hidden = true;
    $("editorPanel").hidden = true;
    $("jobPanel").hidden = true;
    $("fileList").textContent = "";
}

async function authenticate(endpoint) {
    const userId = $("userId").value.trim();
    const password = $("password").value;
    try {
        await api(endpoint, { method: "POST", body: JSON.stringify({ userId, password }) });
        if (endpoint === "/api/users") {
            await api("/api/session", { method: "POST", body: JSON.stringify({ userId, password }) });
        }
        $("authStatus").textContent = "";
        showAuthed(userId);
    } catch (error) {
        $("authStatus").textContent = error.message;
    }
}

async function logout() {
    try {
        await api("/api/session", { method: "DELETE" });
    } catch {
        /* 忽略 */
    }
    showAnonymous();
}

// ---- 工程与文件 ------------------------------------------------------------
async function loadProjects() {
    const data = await api("/api/projects");
    state.projects = data.projects ?? [];
    const select = $("projectSelect");
    select.textContent = "";
    for (const project of state.projects) {
        const option = document.createElement("option");
        option.value = project.projectId;
        option.textContent = `${project.name ?? project.projectId} (${project.projectId})`;
        select.append(option);
    }
    if (state.projects.length === 0) {
        state.project = null;
        $("fileList").textContent = "";
        return;
    }
    state.project = select.value || state.projects[0].projectId;
    select.value = state.project;
    await loadFiles();
}

async function createProject() {
    const projectId = $("newProject").value.trim();
    if (!projectId) return;
    try {
        await api("/api/projects", { method: "POST", body: JSON.stringify({ projectId }) });
        $("newProject").value = "";
        await loadProjects();
        $("projectSelect").value = projectId;
        state.project = projectId;
        await loadFiles();
    } catch (error) {
        setStatus(`新建工程失败：${error.message}`);
    }
}

async function loadFiles() {
    if (!state.project) return;
    const data = await api(`/api/projects/${encodeURIComponent(state.project)}/files`);
    const list = $("fileList");
    list.textContent = "";
    for (const entry of data.files ?? []) {
        if (entry.type !== "file") continue;
        const item = document.createElement("li");
        const button = document.createElement("button");
        button.className = "link";
        button.textContent = entry.path;
        button.addEventListener("click", () => {
            $("filePath").value = entry.path;
            void openFile();
        });
        item.append(button);
        list.append(item);
    }
    if (data.truncated) {
        const note = document.createElement("li");
        note.className = "muted";
        note.textContent = "（文件过多，已截断）";
        list.append(note);
    }
}

async function openFile() {
    if (!state.project) return;
    const path = $("filePath").value.trim();
    if (!path) return;
    try {
        const data = await api(
            `/api/projects/${encodeURIComponent(state.project)}/file?path=${encodeURIComponent(path)}`,
        );
        state.editor.setValue(data.content);
        state.editor.clearHistory();
        setStatus(`已打开 ${path}`);
    } catch (error) {
        setStatus(`打开失败：${error.message}`);
    }
}

async function saveFile() {
    if (!state.project) return;
    const path = $("filePath").value.trim();
    if (!path) {
        setStatus("请先填写文件路径");
        return;
    }
    try {
        await api(
            `/api/projects/${encodeURIComponent(state.project)}/file?path=${encodeURIComponent(path)}`,
            { method: "PUT", body: JSON.stringify({ content: state.editor.getValue() }) },
        );
        setStatus(`已保存 ${path}`);
        await loadFiles();
    } catch (error) {
        setStatus(`保存失败：${error.message}`);
    }
}

// ---- 结构树与诊断 ----------------------------------------------------------
function setAnalysisStatus(text) {
    $("analysisStatus").textContent = text;
}

async function analyze(kind) {
    if (!state.project) return;
    const path = $("filePath").value.trim();
    if (!path) {
        setAnalysisStatus("请先选择文件");
        return;
    }
    setAnalysisStatus("分析中…");
    try {
        const data = await api(
            `/api/projects/${encodeURIComponent(state.project)}/${kind}`,
            { method: "POST", body: JSON.stringify({ path }) },
        );
        if (kind === "structure") renderStructure(data);
        else renderDiagnostics(data);
        setAnalysisStatus("");
    } catch (error) {
        if (error.status === 501) {
            setAnalysisStatus("C++ 侧尚未实现该命令（P6），接口已就绪");
            if (kind === "structure") $("structureTree").textContent = "";
            else $("diagnostics").textContent = "";
        } else {
            setAnalysisStatus(`失败：${error.message}`);
        }
    }
}

function renderStructure(data) {
    const list = $("structureTree");
    list.textContent = "";
    const nodes = Array.isArray(data?.nodes) ? data.nodes : [];
    if (nodes.length === 0) {
        const item = document.createElement("li");
        item.className = "muted";
        item.textContent = "（空）";
        list.append(item);
        return;
    }

    const byId = new Map(nodes.map((node) => [node.id ?? node.name, node]));
    const childIds = new Set();
    for (const node of nodes) {
        for (const child of node.children ?? []) childIds.add(child);
    }
    const roots = nodes.filter((node) => !childIds.has(node.id ?? node.name));

    const build = (node, parent) => {
        const item = document.createElement("li");
        const label = document.createElement("button");
        label.className = "link";
        label.textContent = `${node.kind ?? "node"} ${node.name ?? ""}`.trim();
        const line = node.range?.start?.line;
        if (typeof line === "number") {
            label.addEventListener("click", () => {
                state.editor.setCursor({ line: line - 1, ch: (node.range.start.col ?? 1) - 1 });
                state.editor.focus();
            });
        }
        item.append(label);
        if (typeof line === "number") {
            const where = document.createElement("span");
            where.className = "muted";
            where.textContent = `  ${line}:${node.range.start.col ?? 1}`;
            item.append(where);
        }
        const children = (node.children ?? []).map((id) => byId.get(id)).filter(Boolean);
        if (children.length > 0) {
            const sub = document.createElement("ul");
            for (const child of children) build(child, sub);
            item.append(sub);
        }
        parent.append(item);
    };

    for (const root of roots) build(root, list);
}

function renderDiagnostics(data) {
    const list = $("diagnostics");
    list.textContent = "";
    const diagnostics = Array.isArray(data?.diagnostics) ? data.diagnostics : [];
    if (diagnostics.length === 0) {
        const item = document.createElement("li");
        item.className = "muted";
        item.textContent = "无问题";
        list.append(item);
        return;
    }
    for (const diagnostic of diagnostics) {
        const item = document.createElement("li");
        item.className = `diag-${diagnostic.severity ?? "info"}`;
        const label = document.createElement("button");
        label.className = "link";
        label.textContent =
            `${diagnostic.severity ?? "info"} ${diagnostic.code ?? ""} ${diagnostic.message ?? ""}`.trim();
        if (typeof diagnostic.line === "number") {
            label.addEventListener("click", () => {
                state.editor.setCursor({
                    line: Math.max(0, diagnostic.line - 1),
                    ch: Math.max(0, (diagnostic.col ?? 1) - 1),
                });
                state.editor.focus();
            });
        }
        item.append(label);
        list.append(item);
    }
}

// ---- 任务 ------------------------------------------------------------------
function renderEvent(event) {
    switch (event.type) {
        case "log":
            appendLog(`[${event.level}] ${event.line}`);
            break;
        case "progress":
            appendLog(`[进度] ${event.percent}% ${event.status ?? ""}`);
            break;
        case "diagnostic":
            appendLog(`[诊断] ${event.severity} ${event.code}: ${event.message}`);
            break;
        case "artifact":
            appendLog(`[产物] ${event.path} (${event.sha256.slice(0, 12)}…)`);
            break;
        case "result":
            appendLog(`[结束] state=${event.state} exit=${event.exitCode}`);
            break;
        default:
            appendLog(JSON.stringify(event));
    }
}

function closeStream() {
    if (state.source) {
        state.source.close();
        state.source = null;
    }
}

async function submitJob() {
    if (!state.project) {
        setStatus("请先选择工程");
        return;
    }
    let params;
    try {
        params = JSON.parse($("params").value);
    } catch (error) {
        setStatus(`参数不是合法 JSON：${error.message}`);
        return;
    }

    $("submit").disabled = true;
    $("cancel").disabled = true;
    $("log").textContent = "";
    setStatus("提交中…");

    try {
        const data = await api("/api/jobs", {
            method: "POST",
            body: JSON.stringify({
                projectId: state.project,
                jobType: $("jobType").value,
                params,
            }),
        });
        state.jobId = data.id;
        setStatus(`任务 ${data.id} 已提交`);
        $("cancel").disabled = false;

        closeStream();
        const source = new EventSource(`/api/jobs/${data.id}/events`);
        state.source = source;
        source.onmessage = (message) => {
            try {
                renderEvent(JSON.parse(message.data));
            } catch {
                appendLog(message.data);
            }
        };
        source.addEventListener("end", (message) => {
            const payload = JSON.parse(message.data || "{}");
            setStatus(`任务结束：${payload.state ?? "unknown"}`);
            appendLog(`[完成] ${payload.state ?? "unknown"}`);
            closeStream();
            $("submit").disabled = false;
            $("cancel").disabled = true;
            void refreshHealth();
        });
        source.onerror = () => appendLog("[连接] 事件流中断，浏览器会自动重连");
    } catch (error) {
        setStatus(`提交失败：${error.message}`);
        $("submit").disabled = false;
    }
}

async function cancelJob() {
    if (!state.jobId) return;
    $("cancel").disabled = true;
    try {
        await api(`/api/jobs/${state.jobId}/cancel`, { method: "POST" });
        setStatus("已请求取消");
    } catch (error) {
        setStatus(`取消失败：${error.message}`);
    }
}

// ---- 绑定与启动 ------------------------------------------------------------
initEditor();
$("params").value = JSON.stringify(DEFAULT_PARAMS, null, 2);

$("login").addEventListener("click", () => authenticate("/api/session"));
$("register").addEventListener("click", () => authenticate("/api/users"));
$("logout").addEventListener("click", logout);
$("createProject").addEventListener("click", createProject);
$("projectSelect").addEventListener("change", async (event) => {
    state.project = event.target.value;
    await loadFiles();
});
$("openFile").addEventListener("click", openFile);
$("saveFile").addEventListener("click", saveFile);
$("analyzeStructure").addEventListener("click", () => analyze("structure"));
$("analyzeLint").addEventListener("click", () => analyze("lint"));
$("submit").addEventListener("click", submitJob);
$("cancel").addEventListener("click", cancelJob);

async function boot() {
    await refreshHealth();
    try {
        const session = await api("/api/session");
        showAuthed(session.userId);
    } catch {
        showAnonymous();
    }
}

void boot();
setInterval(() => void refreshHealth(), 5000);
