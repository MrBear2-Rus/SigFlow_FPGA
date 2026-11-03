// =============================================================================
// 页签：<div data-tabs> 下 .tab-list > .tab[data-tab] 与 .tab-panels > .tab-panel[data-panel]
// =============================================================================

export function initTabs(root = document) {
    root.querySelectorAll("[data-tabs]").forEach(setup);
}

function setup(container) {
    const tabs = [...container.querySelectorAll(":scope > .tab-list > .tab")];
    const panels = [...container.querySelectorAll(":scope > .tab-panels > .tab-panel")];
    if (tabs.length === 0) return;

    const select = (name, emit = true) => {
        for (const tab of tabs) {
            const active = tab.dataset.tab === name;
            tab.setAttribute("aria-selected", String(active));
            tab.tabIndex = active ? 0 : -1;
        }
        for (const panel of panels) {
            panel.dataset.active = String(panel.dataset.panel === name);
        }
        if (emit) {
            container.dispatchEvent(
                new CustomEvent("tab-change", { detail: { name }, bubbles: true }),
            );
        }
    };

    for (const tab of tabs) {
        tab.addEventListener("click", () => select(tab.dataset.tab));
        tab.addEventListener("keydown", (event) => {
            if (event.key !== "ArrowRight" && event.key !== "ArrowLeft") return;
            const index = tabs.indexOf(tab);
            const next = (index + (event.key === "ArrowRight" ? 1 : -1) + tabs.length) % tabs.length;
            tabs[next].focus();
            select(tabs[next].dataset.tab);
        });
    }

    const initial = container.dataset.active ?? tabs.find((t) => t.dataset.tab)?.dataset.tab;
    if (initial) select(initial, false);
}
