// 画布编辑器：缩放/平移、框选多选、撤销/重做、元件属性更新。

import { api } from "../lib/api.js";

const SVG_NS = "http://www.w3.org/2000/svg";
const BOARD_W = 2000;
const BOARD_H = 1200;
const MIN_VIEW_W = 200;
const MAX_VIEW_W = 4000;
const HISTORY_LIMIT = 50;

export class CanvasEditor {
    constructor(host, { onChange, onSelect, onLog, onView } = {}) {
        this.host = host;
        this.onChange = onChange ?? (() => {});
        this.onSelect = onSelect ?? (() => {});
        this.onLog = onLog ?? (() => {});
        this.onView = onView ?? (() => {});

        this.library = new Map();
        this.layout = { schema: "sigflow.canvas.v1", elements: [], wires: [] };
        this.selected = null;
        this.selection = new Set();
        this.wireMode = false;
        this.pendingPin = null;
        this.drag = null;
        this.pan = null;
        this.marquee = null;
        this.spaceDown = false;
        this.viewBox = { x: 0, y: 0, w: BOARD_W, h: BOARD_H };
        this.undoStack = [];
        this.redoStack = [];

        this.#build();
    }

    get dirtyHint() {
        return this.layout.elements.length;
    }

    get zoomPercent() {
        return Math.round((BOARD_W / this.viewBox.w) * 100);
    }

    setLibrary(templates) {
        this.library = new Map(templates.map((template) => [template.type, template]));
        this.render();
    }

    async load(projectId) {
        const data = await api.get(`/api/projects/${encodeURIComponent(projectId)}/canvas`);
        this.layout = {
            schema: "sigflow.canvas.v1",
            elements: Array.isArray(data.elements) ? data.elements : [],
            wires: Array.isArray(data.wires) ? data.wires : [],
        };
        this.selected = null;
        this.selection.clear();
        this.undoStack = [];
        this.redoStack = [];
        this.resetView();
        this.render();
    }

    async save(projectId) {
        await api.put(`/api/projects/${encodeURIComponent(projectId)}/canvas`, this.layout);
        this.onLog(`画布已保存：${this.layout.elements.length} 个元件、${this.layout.wires.length} 条连线`);
    }

    // ---- 历史 ---------------------------------------------------------------
    pushHistory() {
        this.undoStack.push(JSON.stringify(this.layout));
        if (this.undoStack.length > HISTORY_LIMIT) this.undoStack.shift();
        this.redoStack = [];
    }

    undo() {
        if (this.undoStack.length === 0) return;
        this.redoStack.push(JSON.stringify(this.layout));
        this.layout = JSON.parse(this.undoStack.pop());
        this.#resetSelection();
        this.onChange();
        this.render();
        this.onLog("撤销");
    }

    redo() {
        if (this.redoStack.length === 0) return;
        this.undoStack.push(JSON.stringify(this.layout));
        this.layout = JSON.parse(this.redoStack.pop());
        this.#resetSelection();
        this.onChange();
        this.render();
        this.onLog("重做");
    }

    // ---- 编辑 ---------------------------------------------------------------
    addElement(type) {
        const template = this.library.get(type);
        if (!template) return;
        this.pushHistory();
        const id = `e${Date.now().toString(36)}${Math.floor(Math.random() * 1000)}`;
        const offset = (this.layout.elements.length % 8) * 24;
        const element = {
            id,
            type,
            x: 120 + offset,
            y: 100 + offset,
            label: `${type}_${this.layout.elements.length + 1}`,
        };
        this.layout.elements.push(element);
        this.selected = { kind: "element", id };
        this.selection = new Set([id]);
        this.onChange();
        this.onSelect(element);
        this.render();
    }

    updateElement(id, patch) {
        const element = this.layout.elements.find((item) => item.id === id);
        if (!element) return;
        const allowed = {};
        if (typeof patch.label === "string") allowed.label = patch.label;
        if (Number.isFinite(patch.x)) allowed.x = Math.max(0, patch.x);
        if (Number.isFinite(patch.y)) allowed.y = Math.max(0, patch.y);
        this.pushHistory();
        Object.assign(element, allowed);
        this.onChange();
        this.onSelect(element);
        this.render();
    }

    deleteSelected() {
        if (this.selection.size > 0) {
            this.pushHistory();
            const ids = new Set(this.selection);
            this.layout.elements = this.layout.elements.filter((item) => !ids.has(item.id));
            this.layout.wires = this.layout.wires.filter(
                (wire) => !ids.has(wire.from.element) && !ids.has(wire.to.element),
            );
            this.#resetSelection();
            this.onChange();
            this.render();
            return;
        }
        if (this.selected?.kind === "wire") {
            this.pushHistory();
            this.layout.wires = this.layout.wires.filter((wire) => wire.id !== this.selected.id);
            this.selected = null;
            this.onChange();
            this.render();
        }
    }

    setWireMode(enabled) {
        this.wireMode = enabled;
        this.pendingPin = null;
    }

    // ---- 视图 ---------------------------------------------------------------
    zoomBy(factor, clientX, clientY) {
        const rect = this.svg.getBoundingClientRect();
        const anchor = this.#toBoard({
            clientX: clientX ?? rect.left + rect.width / 2,
            clientY: clientY ?? rect.top + rect.height / 2,
        });
        const width = Math.min(MAX_VIEW_W, Math.max(MIN_VIEW_W, this.viewBox.w / factor));
        const height = width * (BOARD_H / BOARD_W);
        const scale = Math.min(rect.width / width, rect.height / height);
        const offsetX = (rect.width - width * scale) / 2;
        const offsetY = (rect.height - height * scale) / 2;
        const px = (clientX ?? rect.left + rect.width / 2) - rect.left;
        const py = (clientY ?? rect.top + rect.height / 2) - rect.top;
        this.viewBox = {
            x: anchor.x - (px - offsetX) / scale,
            y: anchor.y - (py - offsetY) / scale,
            w: width,
            h: height,
        };
        this.#applyView();
    }

    resetView() {
        this.viewBox = { x: 0, y: 0, w: BOARD_W, h: BOARD_H };
        this.#applyView();
    }

    // ---- 内部 ---------------------------------------------------------------
    #build() {
        this.svg = document.createElementNS(SVG_NS, "svg");
        this.svg.setAttribute("viewBox", `0 0 ${BOARD_W} ${BOARD_H}`);
        this.svg.setAttribute("class", "ws-board");
        this.svg.setAttribute("preserveAspectRatio", "xMidYMid meet");

        const defs = document.createElementNS(SVG_NS, "defs");
        const pattern = document.createElementNS(SVG_NS, "pattern");
        pattern.setAttribute("id", "ws-grid");
        pattern.setAttribute("width", "20");
        pattern.setAttribute("height", "20");
        pattern.setAttribute("patternUnits", "userSpaceOnUse");
        const gridPath = document.createElementNS(SVG_NS, "path");
        gridPath.setAttribute("d", "M 20 0 L 0 0 0 20");
        gridPath.setAttribute("fill", "none");
        gridPath.setAttribute("stroke", "var(--grid)");
        gridPath.setAttribute("stroke-width", "1");
        pattern.append(gridPath);
        defs.append(pattern);

        const background = document.createElementNS(SVG_NS, "rect");
        background.setAttribute("width", String(BOARD_W));
        background.setAttribute("height", String(BOARD_H));
        background.setAttribute("fill", "url(#ws-grid)");

        this.wireLayer = document.createElementNS(SVG_NS, "g");
        this.nodeLayer = document.createElementNS(SVG_NS, "g");
        this.overlay = document.createElementNS(SVG_NS, "g");
        this.svg.append(defs, background, this.wireLayer, this.nodeLayer, this.overlay);
        this.host.append(this.svg);

        this.svg.addEventListener("wheel", (event) => {
            event.preventDefault();
            this.zoomBy(event.deltaY < 0 ? 1.12 : 1 / 1.12, event.clientX, event.clientY);
        }, { passive: false });
        this.svg.addEventListener("pointerdown", (event) => this.#onBackgroundDown(event));
        this.svg.addEventListener("pointermove", (event) => this.#onPointerMove(event));
        this.svg.addEventListener("pointerup", (event) => this.#onPointerUp(event));
        this.svg.addEventListener("pointerleave", () => { this.pan = null; });

        this.host.tabIndex = 0;
        this.host.addEventListener("keydown", (event) => {
            if (event.key === "Delete" || event.key === "Backspace") {
                if (event.target !== this.host) return;
                event.preventDefault();
                this.deleteSelected();
            } else if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "z") {
                event.preventDefault();
                event.shiftKey ? this.redo() : this.undo();
            } else if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "y") {
                event.preventDefault();
                this.redo();
            } else if (event.key === " ") {
                this.spaceDown = true;
            }
        });
        this.host.addEventListener("keyup", (event) => {
            if (event.key === " ") this.spaceDown = false;
        });
    }

    #toBoard(event) {
        const point = this.svg.createSVGPoint();
        point.x = event.clientX;
        point.y = event.clientY;
        const matrix = this.svg.getScreenCTM();
        if (!matrix) return { x: 0, y: 0 };
        const board = point.matrixTransform(matrix.inverse());
        return { x: board.x, y: board.y };
    }

    #applyView() {
        this.svg.setAttribute(
            "viewBox",
            `${this.viewBox.x} ${this.viewBox.y} ${this.viewBox.w} ${this.viewBox.h}`,
        );
        this.onView(this.zoomPercent);
    }

    #resetSelection() {
        this.selected = null;
        this.selection.clear();
        this.onSelect(null);
    }

    #onBackgroundDown(event) {
        this.host.focus?.();
        if (event.button === 1 || (event.button === 0 && this.spaceDown)) {
            event.preventDefault();
            this.pan = {
                startX: event.clientX,
                startY: event.clientY,
                origin: { x: this.viewBox.x, y: this.viewBox.y },
            };
            return;
        }
        if (event.button !== 0 || this.wireMode) return;
        if (event.target !== this.svg && event.target !== this.wireLayer && event.target !== this.overlay) {
            return;
        }
        const start = this.#toBoard(event);
        this.marquee = { start, current: start, moved: false };
        if (!event.shiftKey) this.selection.clear();
        this.selected = null;
        this.onSelect(null);
    }

    #onPointerMove(event) {
        if (this.pan) {
            const rect = this.svg.getBoundingClientRect();
            const scale = Math.min(rect.width / this.viewBox.w, rect.height / this.viewBox.h);
            this.viewBox.x = this.pan.origin.x - (event.clientX - this.pan.startX) / scale;
            this.viewBox.y = this.pan.origin.y - (event.clientY - this.pan.startY) / scale;
            this.#applyView();
            return;
        }
        if (this.marquee) {
            this.marquee.current = this.#toBoard(event);
            if (Math.abs(this.marquee.current.x - this.marquee.start.x) > 2 ||
                Math.abs(this.marquee.current.y - this.marquee.start.y) > 2) {
                this.marquee.moved = true;
            }
            this.#drawMarquee();
            return;
        }
        if (!this.drag) return;
        const point = this.#toBoard(event);
        const dx = point.x - this.drag.start.x;
        const dy = point.y - this.drag.start.y;
        if (!this.drag.moved && (Math.abs(dx) > 1 || Math.abs(dy) > 1)) {
            this.pushHistory();
            this.drag.moved = true;
        }
        for (const [id, origin] of this.drag.origins) {
            const element = this.layout.elements.find((item) => item.id === id);
            if (!element) continue;
            element.x = Math.max(0, origin.x + dx);
            element.y = Math.max(0, origin.y + dy);
        }
        this.onChange();
        this.render();
    }

    #onPointerUp(event) {
        if (this.pan) {
            this.pan = null;
            return;
        }
        if (this.marquee) {
            const marquee = this.marquee;
            this.marquee = null;
            this.#clearMarquee();
            if (marquee.moved) {
                this.#selectInRect(marquee);
                this.render();
            } else if (!event.shiftKey) {
                this.selection.clear();
                this.selected = null;
                this.onSelect(null);
                this.render();
            }
            return;
        }
        if (this.drag) {
            this.drag = null;
            this.onChange();
        }
    }

    #selectInRect(marquee) {
        const x0 = Math.min(marquee.start.x, marquee.current.x);
        const y0 = Math.min(marquee.start.y, marquee.current.y);
        const x1 = Math.max(marquee.start.x, marquee.current.x);
        const y1 = Math.max(marquee.start.y, marquee.current.y);
        for (const element of this.layout.elements) {
            const template = this.library.get(element.type);
            const width = template?.bounds?.[0] ?? 80;
            const height = template?.bounds?.[1] ?? 60;
            const hit = element.x < x1 && element.x + width > x0 &&
                element.y < y1 && element.y + height > y0;
            if (hit) this.selection.add(element.id);
        }
        const first = this.layout.elements.find((item) => this.selection.has(item.id));
        this.selected = first ? { kind: "element", id: first.id } : null;
        this.onSelect(first ?? null);
        this.onLog(`框选 ${this.selection.size} 个元件`);
    }

    #drawMarquee() {
        this.#clearMarquee();
        const x = Math.min(this.marquee.start.x, this.marquee.current.x);
        const y = Math.min(this.marquee.start.y, this.marquee.current.y);
        const width = Math.abs(this.marquee.current.x - this.marquee.start.x);
        const height = Math.abs(this.marquee.current.y - this.marquee.start.y);
        const rect = document.createElementNS(SVG_NS, "rect");
        rect.setAttribute("x", x);
        rect.setAttribute("y", y);
        rect.setAttribute("width", width);
        rect.setAttribute("height", height);
        rect.setAttribute("class", "ws-marquee");
        this.overlay.append(rect);
    }

    #clearMarquee() {
        this.overlay.textContent = "";
    }

    #pinAbsolute(element, kind, index) {
        const template = this.library.get(element.type);
        const pins = kind === "in" ? template?.inputPins : template?.outputPins;
        const pin = pins?.[index];
        return pin ? { x: element.x + pin.x, y: element.y + pin.y } : { x: element.x, y: element.y };
    }

    render() {
        this.nodeLayer.textContent = "";
        this.wireLayer.textContent = "";
        for (const wire of this.layout.wires) this.#drawWire(wire);
        for (const element of this.layout.elements) this.#drawElement(element);
    }

    #drawElement(element) {
        const template = this.library.get(element.type);
        if (!template) return;

        const group = document.createElementNS(SVG_NS, "g");
        group.setAttribute("transform", `translate(${element.x},${element.y})`);
        if (this.selection.has(element.id)) group.classList.add("selected");

        for (const shape of template.shapes ?? []) {
            const node = shapeToSvg(shape);
            if (node) group.append(node);
        }

        (template.inputPins ?? []).forEach((pin, index) => {
            group.append(this.#pin(element, "in", index, pin));
        });
        (template.outputPins ?? []).forEach((pin, index) => {
            group.append(this.#pin(element, "out", index, pin));
        });

        const label = document.createElementNS(SVG_NS, "text");
        label.setAttribute("x", 0);
        label.setAttribute("y", (template.bounds?.[1] ?? 80) + 16);
        label.setAttribute("class", "ws-node-label");
        label.textContent = element.label ?? element.type;
        group.append(label);

        group.addEventListener("pointerdown", (event) => {
            if (this.wireMode || event.button !== 0 || this.spaceDown) return;
            event.preventDefault();
            event.stopPropagation();
            this.host.focus?.();
            if (event.shiftKey) {
                if (this.selection.has(element.id)) this.selection.delete(element.id);
                else this.selection.add(element.id);
            } else if (!this.selection.has(element.id)) {
                this.selection = new Set([element.id]);
            }
            this.selected = { kind: "element", id: element.id };
            this.onSelect(element);
            const origins = new Map();
            for (const id of this.selection) {
                const item = this.layout.elements.find((entry) => entry.id === id);
                if (item) origins.set(id, { x: item.x, y: item.y });
            }
            this.drag = { origins, start: this.#toBoard(event), moved: false };
            this.render();
        });

        this.nodeLayer.append(group);
    }

    #pin(element, kind, index, pin) {
        const circle = document.createElementNS(SVG_NS, "circle");
        circle.setAttribute("cx", pin.x);
        circle.setAttribute("cy", pin.y);
        circle.setAttribute("r", 4);
        circle.setAttribute("class", `ws-pin ws-pin-${kind}`);
        circle.addEventListener("pointerdown", (event) => event.stopPropagation());
        circle.addEventListener("click", (event) => {
            event.stopPropagation();
            if (!this.wireMode) return;
            this.#onPin(element, kind, index);
        });
        return circle;
    }

    #onPin(element, kind, index) {
        if (!this.pendingPin) {
            this.pendingPin = { element: element.id, kind, index, label: element.label };
            this.onLog(`连线起点：${element.label ?? element.type}.${kind}${index}`);
            return;
        }
        const first = this.pendingPin;
        this.pendingPin = null;
        if (first.kind === kind) {
            this.onLog("请连接一个输出与一个输入");
            return;
        }
        const from = first.kind === "out" ? first : { element: element.id, kind, index };
        const to = first.kind === "in" ? first : { element: element.id, kind, index };
        this.pushHistory();
        this.layout.wires.push({
            id: `w${Date.now().toString(36)}`,
            from: { element: from.element, port: `out${from.index}`, kind: "out", index: from.index },
            to: { element: to.element, port: `in${to.index}`, kind: "in", index: to.index },
        });
        this.selected = { kind: "wire", id: this.layout.wires.at(-1).id };
        this.onChange();
        this.render();
        this.onLog("已连线");
    }

    #drawWire(wire) {
        const from = this.layout.elements.find((item) => item.id === wire.from.element);
        const to = this.layout.elements.find((item) => item.id === wire.to.element);
        if (!from || !to) return;
        const a = this.#pinAbsolute(from, wire.from.kind ?? "out", wire.from.index ?? 0);
        const b = this.#pinAbsolute(to, wire.to.kind ?? "in", wire.to.index ?? 0);
        const mid = (a.x + b.x) / 2;
        const path = document.createElementNS(SVG_NS, "path");
        path.setAttribute("d", `M ${a.x} ${a.y} H ${mid} V ${b.y} H ${b.x}`);
        path.setAttribute("class", "ws-wire");
        if (this.selected?.kind === "wire" && this.selected.id === wire.id) {
            path.classList.add("selected");
        }
        path.addEventListener("pointerdown", (event) => {
            event.stopPropagation();
            this.selected = { kind: "wire", id: wire.id };
            this.selection.clear();
            this.onSelect(null);
            this.render();
        });
        this.wireLayer.append(path);
    }
}

function shapeToSvg(shape) {
    const stroke = shape.color ?? "#878c99";
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
