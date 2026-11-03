// 结构树与诊断的 DOM 渲染

/**
 * @param {HTMLElement} host
 * @param {Array} nodes
 * @param {(line: number, col: number) => void} onJump
 */
export function renderStructure(host, nodes, onJump) {
    host.textContent = "";
    if (!Array.isArray(nodes) || nodes.length === 0) {
        host.append(emptyLine("没有结构节点"));
        return;
    }

    const byId = new Map(nodes.map((node) => [node.id, node]));
    const childIds = new Set();
    for (const node of nodes) for (const child of node.children ?? []) childIds.add(child);
    const roots = nodes.filter((node) => !childIds.has(node.id));

    const list = document.createElement("ul");
    list.className = "structure-tree";
    const build = (node, parent) => {
        const item = document.createElement("li");
        const row = document.createElement("button");
        row.type = "button";
        row.className = "structure-row";
        const kind = document.createElement("span");
        kind.className = "structure-kind";
        kind.textContent = node.kind ?? "node";
        const name = document.createElement("span");
        name.textContent = node.name || "(匿名)";
        row.append(kind, name);
        if (node.range?.start?.line) {
            const where = document.createElement("span");
            where.className = "structure-pos";
            where.textContent = `${node.range.start.line}:${node.range.start.col ?? 1}`;
            row.append(where);
        }
        row.addEventListener("click", () => {
            onJump(node.range?.start?.line ?? 1, node.range?.start?.col ?? 1);
        });
        item.append(row);

        const children = (node.children ?? []).map((id) => byId.get(id)).filter(Boolean);
        if (children.length > 0) {
            const sub = document.createElement("ul");
            for (const child of children) build(child, sub);
            item.append(sub);
        }
        parent.append(item);
    };
    for (const root of roots) build(root, list);
    host.append(list);
}

/**
 * @param {HTMLElement} host
 * @param {Array} diagnostics
 * @param {(line: number, col: number) => void} onJump
 */
export function renderDiagnostics(host, diagnostics, onJump) {
    host.textContent = "";
    if (!Array.isArray(diagnostics) || diagnostics.length === 0) {
        host.append(emptyLine("无问题"));
        return;
    }
    const list = document.createElement("ul");
    list.className = "diag-list";
    for (const diagnostic of diagnostics) {
        const item = document.createElement("li");
        item.className = `diag-${diagnostic.severity ?? "info"}`;
        const row = document.createElement("button");
        row.type = "button";
        row.className = "diag-row";
        const where = document.createElement("span");
        where.className = "structure-pos";
        where.textContent = `${diagnostic.line ?? "?"}:${diagnostic.col ?? 1}`;
        const message = document.createElement("span");
        message.textContent = `${diagnostic.code ?? ""} ${diagnostic.message ?? ""}`.trim();
        row.append(where, message);
        row.addEventListener("click", () => {
            onJump(diagnostic.line ?? 1, diagnostic.col ?? 1);
        });
        item.append(row);
        list.append(item);
    }
    host.append(list);
}

function emptyLine(text) {
    const node = document.createElement("div");
    node.className = "muted small";
    node.textContent = text;
    return node;
}
