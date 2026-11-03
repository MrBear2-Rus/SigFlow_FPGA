// 工程管理页（U3）：列表、新建（空白 / 示例）、打开、删除

import { api } from "./lib/api.js";
import { confirmDialog, showDialog } from "./lib/dialog.js";
import { toast } from "./lib/toast.js";

const $ = (id) => document.getElementById(id);
const OPEN_PAGE = "/pages/workspace.html";
const TEMPLATES = [
    { value: "blank", label: "空白工程" },
    { value: "sample", label: "示例：1 Hz 闪烁灯" },
];

let currentUser = null;
let projects = [];

function formatTime(iso) {
    if (!iso) return "—";
    const date = new Date(iso);
    if (Number.isNaN(date.getTime())) return "—";
    const pad = (value) => String(value).padStart(2, "0");
    return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())} ` +
        `${pad(date.getHours())}:${pad(date.getMinutes())}`;
}

function openProject(projectId) {
    location.href = `${OPEN_PAGE}?project=${encodeURIComponent(projectId)}`;
}

function render() {
    const list = $("list");
    list.textContent = "";
    $("empty").classList.toggle("hidden", projects.length > 0);

    for (const project of projects) {
        const card = document.createElement("article");
        card.className = "project-card";

        const head = document.createElement("div");
        head.className = "project-card-head";
        const title = document.createElement("h3");
        title.textContent = project.name ?? project.projectId;
        title.title = project.name ?? project.projectId;
        const chip = document.createElement("span");
        chip.className = "chip";
        chip.textContent = project.template === "sample" ? "示例" : "工程";
        head.append(title, chip);

        const meta = document.createElement("div");
        meta.className = "project-meta";
        const idLine = document.createElement("span");
        idLine.append("ID：", Object.assign(document.createElement("code"), { textContent: project.projectId }));
        const created = document.createElement("span");
        created.textContent = `创建：${formatTime(project.createdAt)}`;
        const updated = document.createElement("span");
        updated.textContent = `更新：${formatTime(project.updatedAt)}`;
        meta.append(idLine, created, updated);

        const actions = document.createElement("div");
        actions.className = "project-actions";
        const open = document.createElement("button");
        open.className = "btn btn-primary grow";
        open.textContent = "打开";
        open.addEventListener("click", () => openProject(project.projectId));
        const remove = document.createElement("button");
        remove.className = "btn btn-danger";
        remove.textContent = "删除";
        remove.addEventListener("click", () => removeProject(project));
        actions.append(open, remove);

        card.append(head, meta, actions);
        list.append(card);
    }
}

async function load() {
    const data = await api.get("/api/projects");
    projects = data.projects ?? [];
    render();
}

async function removeProject(project) {
    const ok = await confirmDialog({
        title: "删除工程",
        message: `确定删除「${project.name ?? project.projectId}」？工程内的源码、画布与任务记录都会一并删除，且不可恢复。`,
        confirmText: "删除",
        danger: true,
    });
    if (!ok) return;
    try {
        await api.del(`/api/projects/${encodeURIComponent(project.projectId)}`);
        toast("已删除", { type: "error" });
        await load();
    } catch (error) {
        toast(`删除失败：${error.message}`, { type: "error" });
    }
}

function buildCreateForm() {
    const form = document.createElement("form");
    form.className = "project-form";
    form.addEventListener("submit", (event) => event.preventDefault());

    const idField = document.createElement("div");
    idField.className = "field";
    const idLabel = document.createElement("label");
    idLabel.textContent = "工程 ID";
    const idInput = document.createElement("input");
    idInput.className = "input";
    idInput.placeholder = "例如 tang-nano-demo";
    idInput.required = true;
    const idHint = document.createElement("span");
    idHint.className = "form-hint";
    idHint.textContent = "字母、数字、点、下划线、连字符，1–64 位；创建后不可修改";
    idField.append(idLabel, idInput, idHint);

    const nameField = document.createElement("div");
    nameField.className = "field";
    const nameLabel = document.createElement("label");
    nameLabel.textContent = "显示名称（可选）";
    const nameInput = document.createElement("input");
    nameInput.className = "input";
    nameInput.placeholder = "默认与工程 ID 相同";
    nameField.append(nameLabel, nameInput);

    const templateField = document.createElement("div");
    templateField.className = "field";
    const templateLabel = document.createElement("label");
    templateLabel.textContent = "模板";
    const templateSelect = document.createElement("select");
    templateSelect.className = "input";
    for (const template of TEMPLATES) {
        const option = document.createElement("option");
        option.value = template.value;
        option.textContent = template.label;
        templateSelect.append(option);
    }
    templateField.append(templateLabel, templateSelect);

    const error = document.createElement("p");
    error.className = "form-error";
    error.setAttribute("role", "alert");

    form.append(idField, nameField, templateField, error);
    return { form, idInput, nameInput, templateSelect, error };
}

async function createProject() {
    const { form, idInput, nameInput, templateSelect, error } = buildCreateForm();
    let createdId = null;

    const result = await showDialog({
        title: "新建工程",
        content: form,
        actions: [
            { label: "取消", value: null },
            {
                label: "创建并打开",
                value: "create",
                primary: true,
                onClick: async () => {
                    error.textContent = "";
                    const projectId = idInput.value.trim();
                    if (!/^[A-Za-z0-9._-]{1,64}$/.test(projectId)) {
                        error.textContent = "工程 ID 只能包含字母、数字、点、下划线、连字符，长度 1–64";
                        return false;
                    }
                    try {
                        await api.post("/api/projects", {
                            projectId,
                            name: nameInput.value.trim() || undefined,
                            template: templateSelect.value,
                        });
                        createdId = projectId;
                        return true;
                    } catch (requestError) {
                        error.textContent = requestError.message;
                        return false;
                    }
                },
            },
        ],
    });

    if (result === "create" && createdId) {
        toast("工程已创建", { type: "success" });
        openProject(createdId);
    }
}

async function logout() {
    try {
        await api.del("/api/session");
    } catch {
        /* 忽略 */
    }
    location.href = "/";
}

async function boot() {
    try {
        const session = await api.get("/api/session");
        currentUser = session.userId;
        $("userName").textContent = currentUser;
    } catch {
        location.href = "/";
        return;
    }
    await load();
}

$("newProject").addEventListener("click", createProject);
$("emptyNew").addEventListener("click", createProject);
$("refresh").addEventListener("click", () => load().catch((error) => toast(error.message, { type: "error" })));
$("logout").addEventListener("click", logout);

void boot();
