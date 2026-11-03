// =============================================================================
// 可分栏：<div class="split" data-split [data-axis="y"] [data-key="..."] style="--a:220px">
//   <div class="pane pane-a">…</div><div class="splitter"></div><div class="pane pane-b">…</div>
// 拖动分隔条调整 --a；带 data-key 时把比例存进 localStorage。
// =============================================================================

const MIN_A = 80;
const MIN_B = 120;
const STORAGE_PREFIX = "sigflow.split.";

export function initSplitters(root = document) {
    root.querySelectorAll("[data-split]").forEach(setup);
}

function setup(container) {
    const handle = container.querySelector(":scope > .splitter");
    const paneA = container.querySelector(":scope > .pane-a");
    const paneB = container.querySelector(":scope > .pane-b");
    if (!handle || !paneA || !paneB) return;

    const axis = container.dataset.axis === "y" ? "y" : "x";
    const key = container.dataset.key ? `${STORAGE_PREFIX}${container.dataset.key}` : "";

    restore(container, key, axis);

    let drag = null;

    handle.addEventListener("pointerdown", (event) => {
        if (event.button !== 0) return;
        event.preventDefault();
        handle.setPointerCapture?.(event.pointerId);
        handle.dataset.dragging = "true";
        drag = {
            start: axis === "y" ? event.clientY : event.clientX,
            origin: currentSize(container, axis),
            reverse: container.dataset.reverse === axis,
        };
    });

    handle.addEventListener("pointermove", (event) => {
        if (!drag) return;
        const total = axis === "y" ? container.clientHeight : container.clientWidth;
        let delta = (axis === "y" ? event.clientY : event.clientX) - drag.start;
        if (drag.reverse) delta = -delta;
        const next = clamp(drag.origin + delta, MIN_A, Math.max(MIN_A, total - MIN_B));
        container.style.setProperty("--a", `${Math.round(next)}px`);
    });

    const end = () => {
        if (!drag) return;
        drag = null;
        handle.dataset.dragging = "false";
        save(container, key, axis);
    };
    handle.addEventListener("pointerup", end);
    handle.addEventListener("pointercancel", end);

    // 双击恢复默认比例
    handle.addEventListener("dblclick", () => {
        container.style.removeProperty("--a");
        save(container, key, axis);
    });

    new ResizeObserver(() => clampCurrent(container, axis)).observe(container);
}

function currentSize(container, axis) {
    const value = container.style.getPropertyValue("--a");
    if (value.endsWith("px")) return Number.parseFloat(value);
    const rect = container.getBoundingClientRect();
    return axis === "y" ? rect.height * 0.4 : rect.width * 0.28;
}

function clampCurrent(container, axis) {
    const total = axis === "y" ? container.clientHeight : container.clientWidth;
    const value = currentSize(container, axis);
    const next = clamp(value, MIN_A, Math.max(MIN_A, total - MIN_B));
    container.style.setProperty("--a", `${Math.round(next)}px`);
}

function clamp(value, min, max) {
    return Math.min(Math.max(value, min), max);
}

function save(container, key, axis) {
    if (!key) return;
    const total = axis === "y" ? container.clientHeight : container.clientWidth;
    if (total <= 0) return;
    const ratio = currentSize(container, axis) / total;
    try {
        localStorage.setItem(key, String(ratio));
    } catch {
        /* 隐私模式忽略 */
    }
}

function restore(container, key, axis, attempt = 0) {
    let ratio = Number.NaN;
    if (key) {
        try {
            ratio = Number.parseFloat(localStorage.getItem(key) ?? "");
        } catch {
            ratio = Number.NaN;
        }
    }
    const total = axis === "y" ? container.clientHeight : container.clientWidth;
    if (total <= 0) {
        if (attempt >= 20) return; // 面板一直隐藏时放弃恢复，避免无限重试
        requestAnimationFrame(() => restore(container, key, axis, attempt + 1));
        return;
    }
    if (!Number.isFinite(ratio) || ratio <= 0) {
        // 没有存储值：使用 data-default（0~1 的比例）
        const fallback = Number.parseFloat(container.dataset.default ?? "");
        if (Number.isFinite(fallback) && fallback > 0 && fallback < 1) {
            container.style.setProperty("--a", `${Math.round(fallback * total)}px`);
        }
        return;
    }
    container.style.setProperty("--a", `${Math.round(ratio * total)}px`);
}
