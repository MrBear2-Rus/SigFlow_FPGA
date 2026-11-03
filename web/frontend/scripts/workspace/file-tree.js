// 文件树：把扁平的 /files 结果渲染成目录树

import { iconURL } from "../lib/icons.js";

const EXT_ICON = {
    ".v": "file_v",
    ".sv": "file_v",
    ".vh": "file_v",
    ".svh": "file_v",
    ".md": "file_md",
    ".json": "file_project",
    ".cst": "file_project",
};

export function fileIcon(path) {
    const match = /(\.[A-Za-z0-9]+)$/.exec(path);
    return EXT_ICON[match?.[1]?.toLowerCase()] ?? "file";
}

/**
 * @param {Array<{path: string, type: string}>} entries
 * @param {(path: string) => void} onOpen
 */
export function buildFileTree(entries, onOpen) {
    const root = { children: new Map(), files: [] };
    for (const entry of entries) {
        if (entry.type !== "file") continue;
        const parts = entry.path.split("/");
        let node = root;
        for (const part of parts.slice(0, -1)) {
            if (!node.children.has(part)) {
                node.children.set(part, { name: part, children: new Map(), files: [] });
            }
            node = node.children.get(part);
        }
        node.files.push({ name: parts.at(-1), path: entry.path });
    }

    const list = document.createElement("ul");
    list.className = "file-tree";
    renderNode(root, list, onOpen);
    return list;
}

function renderNode(node, host, onOpen) {
    for (const child of [...node.children.values()].sort((a, b) => a.name.localeCompare(b.name))) {
        const item = document.createElement("li");
        item.className = "file-dir";
        const label = document.createElement("div");
        label.className = "file-row";
        label.append(iconElement("folder"), document.createTextNode(child.name));
        const sub = document.createElement("ul");
        sub.className = "file-tree";
        renderNode(child, sub, onOpen);
        item.append(label, sub);
        host.append(item);
    }

    for (const file of node.files.sort((a, b) => a.name.localeCompare(b.name))) {
        const item = document.createElement("li");
        const button = document.createElement("button");
        button.type = "button";
        button.className = "file-row file-item";
        button.title = file.path;
        button.append(iconElement(fileIcon(file.path)), document.createTextNode(file.name));
        button.addEventListener("click", () => onOpen(file.path));
        item.append(button);
        host.append(item);
    }
}

function iconElement(name) {
    const img = document.createElement("img");
    img.src = iconURL("ui", name);
    img.alt = "";
    return img;
}
