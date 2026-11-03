// 独立窗口的公共初始化：登录校验、工程标题、工作区链接、关闭按钮

import { api } from "./lib/api.js";

export async function initWindow() {
    try {
        await api.get("/api/session");
    } catch {
        location.href = "/";
        throw new Error("未登录");
    }

    const projectId = new URLSearchParams(location.search).get("project");
    const nameEl = document.getElementById("projectName");
    const link = document.getElementById("workspaceLink");
    if (link && projectId) {
        link.href = `/pages/workspace.html?project=${encodeURIComponent(projectId)}`;
    }
    if (projectId) {
        try {
            const project = await api.get(`/api/projects/${encodeURIComponent(projectId)}`);
            if (nameEl) nameEl.textContent = project.name ?? projectId;
            const base = document.title.split(" · ")[0];
            document.title = `${project.name ?? projectId} · ${base}`;
        } catch {
            if (nameEl) nameEl.textContent = "工程不存在";
        }
    }

    document.getElementById("closeWindow")?.addEventListener("click", () => window.close());
    return { projectId };
}
