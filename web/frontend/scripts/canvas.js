// =============================================================================
// 画布编辑（J5）
//
// 数据模型：sigflow.canvas.v1
//   { schema, elements:[{id,type,x,y,label}], wires:[{id,from:{element,port},to:{element,port}}] }
// 坐标：元件左上角；引脚坐标取自 /assets/canvas_elements.json。
// 持久化：<project>/.sigflow/canvas.json（Node 侧校验，JS 不直接碰磁盘）。
// =============================================================================

const SVG_NS = "http://www.w3.org/2000/svg";
const $ = (id) => document.getElementById(id);

const state = {
    project: null,
    library: new Map(),
    layout: { schema: "sigflow.canvas.v1", elements: [], wires: [] },
    selected: null,
    wireMode: false,
    pendingPin: null,
    drag: null,
};

// ---- 请求 ------------------------------------------------------------------
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
        error.status = response.status;
        throw error;
    }
    return data;
}

function setStatus(text) {
    $("status").textContent = text;
}

// ---- 初始化 ----------------------------------------------------------------
async function boot() {
    try {
        const session = await api("/api/session");
        $("account").textContent = `已登录：${session.userId}`;
    } catch {
        $("account").textContent = "未登录";
        setStatus("请先在控制台登录");
        return;
    }

    const library = await (await fetch("/assets/canvas_elements.json")).json();
    for (const template of library) state.library.set(template.type, template);
    renderPalette(library);

    await loadProjects();
}

async function loadProjects() {
    const data = await api("/api/projects");
    const select = $("projectSelect");
    select.textContent = "";
    for (const project of data.projects ?? []) {
        const option = document.createElement("option");
        option.value = project.projectId;
        option.textContent = project.projectId;
        select.append(option);
    }
    if ((data.projects ?? []).length === 0) {
        setStatus("还没有工程，先去控制台新建");
        return;
    }
    state.project = select.value;
    await loadCanvas();
}

function renderPalette(library) {
    const palette = $("palette");
    palette.textContent = "";
    for (const template of library) {
        const button = document.createElement("button");
        button.textContent = template.type;
        button.addEventListener("click", () => addElement(template.type));
        palette.append(button);
    }
}

// ---- 画布持久化 ------------------------------------------------------------
async function loadCanvas() {
    if (!state.project) return;
    try {
        const data = await api(`/api/projects/${encodeURIComponent(state.project)}/canvas`);
        state.layout = {
            schema: "sigflow.canvas.v1",
            elements: Array.isArray(data.elements) ? data.elements : [],
            wires: Array.isArray(data.wires) ? data.wires : [],
        };
        state.selected = null;
        state.pendingPin = null;
        draw();
        setStatus(`已加载 ${state.layout.elements.length} 个元件`);
    } catch (error) {
        setStatus(`加载失败：${error.message}`);
    }
}

async function saveCanvas() {
    if (!state.project) return;
    try {
        await api(`/api/projects/${encodeURIComponent(state.project)}/canvas`, {
            method: "PUT",
            body: JSON.stringify(state.layout),
        });
        setStatus(`已保存 ${state.layout.elements.length} 个元件、${state.layout.wires.length} 条连线`);
    } catch (error) {
        setStatus(`保存失败：${error.message}`);
    }
}

// ---- 元件操作 --------------------------------------------------------------
function addElement(type) {
    const template = state.library.get(type);
    if (!template) return;
    const id = `e${Date.now().toString(36)}${Math.floor(Math.random() * 1000)}`;
    const offset = (state.layout.elements.length % 8) * 24;
    state.layout.elements.push({
        id,
        type,
        x: 120 + offset,
        y: 100 + offset,
        label: `${type}_${state.layout.elements.length + 1}`,
    });
    state.selected = { kind: "element", id };
    draw();
}

function deleteSelected() {
    const selected = state.selected;
    if (!selected) return;
    if (selected.kind === "element") {
        state.layout.elements = state.layout.elements.filter((item) => item.id !== selected.id);
        state.layout.wires = state.layout.wires.filter(
            (wire) => wire.from.element !== selected.id && wire.to.element !== selected.id,
        );
    } else if (selected.kind === "wire") {
        state.layout.wires = state.layout.wires.filter((wire) => wire.id !== selected.id);
    }
    state.selected = null;
    draw();
}

function pinAbsolute(element, kind, index) {
    const template = state.library.get(element.type);
    if (!template) return { x: element.x, y: element.y };
    const pins = kind === "in" ? template.inputPins : template.outputPins;
    const pin = pins?.[index];
    if (!pin) return { x: element.x, y: element.y };
    return { x: element.x + pin.x, y: element.y + pin.y };
}

function portName(kind, index) {
    return `${kind === "in" ? "in" : "out"}${index}`;
}

// ---- 绘制 ------------------------------------------------------------------
function draw() {
    const nodes = $("nodeLayer");
    const wires = $("wireLayer");
    nodes.textContent = "";
    wires.textContent = "";

    for (const wire of state.layout.wires) drawWire(wires, wire);
    for (const element of state.layout.elements) drawElement(nodes, element);
}

function drawElement(layer, element) {
    const template = state.library.get(element.type);
    if (!template) return;

    const group = document.createElementNS(SVG_NS, "g");
    group.setAttribute("transform", `translate(${element.x},${element.y})`);
    group.dataset.id = element.id;
    if (state.selected?.kind === "element" && state.selected.id === element.id) {
        group.classList.add("selected");
    }

    for (const shape of template.shapes ?? []) {
        const node = shapeToSvg(shape);
        if (node) group.append(node);
    }

    (template.inputPins ?? []).forEach((pin, index) => {
        group.append(renderPin(element, "in", index, pin));
    });
    (template.outputPins ?? []).forEach((pin, index) => {
        group.append(renderPin(element, "out", index, pin));
    });

    const label = document.createElementNS(SVG_NS, "text");
    label.setAttribute("x", 0);
    label.setAttribute("y", (template.bounds?.[1] ?? 80) + 16);
    label.setAttribute("class", "node-label");
    label.textContent = element.label ?? element.type;
    group.append(label);

    group.addEventListener("pointerdown", (event) => onElementPointerDown(event, element));
    group.addEventListener("click", (event) => {
        event.stopPropagation();
        if (state.wireMode) return;
        state.selected = { kind: "element", id: element.id };
        draw();
    });

    layer.append(group);
}

function renderPin(element, kind, index, pin) {
    const circle = document.createElementNS(SVG_NS, "circle");
    circle.setAttribute("cx", pin.x);
    circle.setAttribute("cy", pin.y);
    circle.setAttribute("r", 4);
    circle.setAttribute("class", `pin pin-${kind}`);
    circle.dataset.element = element.id;
    circle.dataset.kind = kind;
    circle.dataset.index = String(index);
    circle.addEventListener("click", (event) => {
        event.stopPropagation();
        if (!state.wireMode) return;
        onPinClick(element, kind, index);
    });
    return circle;
}

function drawWire(layer, wire) {
    const from = state.layout.elements.find((item) => item.id === wire.from.element);
    const to = state.layout.elements.find((item) => item.id === wire.to.element);
    if (!from || !to) return;

    const a = pinAbsolute(from, wire.from.kind ?? "out", wire.from.index ?? 0);
    const b = pinAbsolute(to, wire.to.kind ?? "in", wire.to.index ?? 0);
    const mid = (a.x + b.x) / 2;

    const path = document.createElementNS(SVG_NS, "path");
    path.setAttribute("d", `M ${a.x} ${a.y} H ${mid} V ${b.y} H ${b.x}`);
    path.setAttribute("class", "wire");
    if (state.selected?.kind === "wire" && state.selected.id === wire.id) {
        path.classList.add("selected");
    }
    path.addEventListener("click", (event) => {
        event.stopPropagation();
        state.selected = { kind: "wire", id: wire.id };
        draw();
    });
    layer.append(path);
}

function shapeToSvg(shape) {
    const stroke = shape.color ?? "#333333";
    switch (shape.type) {
        case "line": {
            const node = document.createElementNS(SVG_NS, "line");
            node.setAttribute("x1", shape.start.x);
            node.setAttribute("y1", shape.start.y);
            node.setAttribute("x2", shape.end.x);
            node.setAttribute("y2", shape.end.y);
            node.setAttribute("stroke", stroke);
            node.setAttribute("stroke-width", "2");
            return node;
        }
        case "circle": {
            const node = document.createElementNS(SVG_NS, "circle");
            node.setAttribute("cx", shape.center?.x ?? shape.x ?? 0);
            node.setAttribute("cy", shape.center?.y ?? shape.y ?? 0);
            node.setAttribute("r", shape.radius ?? 4);
            node.setAttribute("fill", "none");
            node.setAttribute("stroke", stroke);
            node.setAttribute("stroke-width", "2");
            return node;
        }
        case "ArcShape": {
            const { center, radius } = shape;
            const a0 = (shape.startAngle * Math.PI) / 180;
            const a1 = (shape.endAngle * Math.PI) / 180;
            const x0 = center.x + radius * Math.cos(a0);
            const y0 = center.y + radius * Math.sin(a0);
            const x1 = center.x + radius * Math.cos(a1);
            const y1 = center.y + radius * Math.sin(a1);
            const large = Math.abs(shape.endAngle - shape.startAngle) > 180 ? 1 : 0;
            const sweep = shape.endAngle > shape.startAngle ? 1 : 0;
            const node = document.createElementNS(SVG_NS, "path");
            node.setAttribute("d", `M ${x0} ${y0} A ${radius} ${radius} 0 ${large} ${sweep} ${x1} ${y1}`);
            node.setAttribute("fill", "none");
            node.setAttribute("stroke", stroke);
            node.setAttribute("stroke-width", "2");
            return node;
        }
        case "CubicBezierShape": {
            const { p0, p1, p2, p3 } = shape;
            const node = document.createElementNS(SVG_NS, "path");
            node.setAttribute(
                "d",
                `M ${p0.x} ${p0.y} C ${p1.x} ${p1.y}, ${p2.x} ${p2.y}, ${p3.x} ${p3.y}`,
            );
            node.setAttribute("fill", "none");
            node.setAttribute("stroke", stroke);
            node.setAttribute("stroke-width", "2");
            return node;
        }
        default:
            return null;
    }
}

// ---- 交互 ------------------------------------------------------------------
function onElementPointerDown(event, element) {
    if (state.wireMode) return;
    if (event.button !== 0) return;
    event.preventDefault();
    state.drag = {
        id: element.id,
        startX: event.clientX,
        startY: event.clientY,
        originX: element.x,
        originY: element.y,
    };
    event.target.setPointerCapture?.(event.pointerId);
}

function onPointerMove(event) {
    if (!state.drag) return;
    const element = state.layout.elements.find((item) => item.id === state.drag.id);
    if (!element) return;
    element.x = Math.max(0, state.drag.originX + (event.clientX - state.drag.startX));
    element.y = Math.max(0, state.drag.originY + (event.clientY - state.drag.startY));
    draw();
}

function onPointerUp() {
    state.drag = null;
}

function onPinClick(element, kind, index) {
    if (!state.pendingPin) {
        state.pendingPin = { element: element.id, kind, index };
        setStatus(`已选起点：${element.label ?? element.type}.${portName(kind, index)}`);
        return;
    }
    const first = state.pendingPin;
    state.pendingPin = null;
    if (first.element === element.id && first.kind === kind && first.index === index) {
        setStatus("已取消连线");
        return;
    }
    if (first.kind === kind) {
        setStatus("请连接一个输出与一个输入");
        return;
    }
    const from = first.kind === "out" ? first : { element: element.id, kind, index };
    const to = first.kind === "in" ? first : { element: element.id, kind, index };
    const wire = {
        id: `w${Date.now().toString(36)}`,
        from: { element: from.element, port: portName("out", from.index), kind: "out", index: from.index },
        to: { element: to.element, port: portName("in", to.index), kind: "in", index: to.index },
    };
    state.layout.wires.push(wire);
    state.selected = { kind: "wire", id: wire.id };
    draw();
    setStatus("已连线");
}

function toggleWireMode() {
    state.wireMode = !state.wireMode;
    state.pendingPin = null;
    $("wireMode").setAttribute("aria-pressed", String(state.wireMode));
    $("wireMode").textContent = state.wireMode ? "连线模式（开）" : "连线模式";
    setStatus(state.wireMode ? "依次点击输出引脚与输入引脚" : "");
}

// ---- 绑定 ------------------------------------------------------------------
$("board").addEventListener("pointermove", onPointerMove);
$("board").addEventListener("pointerup", onPointerUp);
$("board").addEventListener("click", () => {
    if (state.selected) {
        state.selected = null;
        draw();
    }
});
$("projectSelect").addEventListener("change", async (event) => {
    state.project = event.target.value;
    await loadCanvas();
});
$("wireMode").addEventListener("click", toggleWireMode);
$("deleteSelected").addEventListener("click", deleteSelected);
$("saveCanvas").addEventListener("click", saveCanvas);
$("reloadCanvas").addEventListener("click", loadCanvas);
window.addEventListener("keydown", (event) => {
    if (event.key === "Delete" || event.key === "Backspace") {
        if (document.activeElement?.tagName === "INPUT") return;
        deleteSelected();
    }
});

void boot();
