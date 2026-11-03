// 调试契约窗口（骨架）：表单实时预览，保存等 C++ P8

import { initWindow } from "./window-common.js";

const $ = (id) => document.getElementById(id);

function updatePreview() {
    const payload = {
        top_module: $("dc-top").value.trim(),
        trigger: $("dc-trigger").value,
        depth: Number.parseInt($("dc-depth").value, 10) || 0,
        probe: $("dc-probe").value.trim(),
    };
    $("dc-preview").textContent = JSON.stringify(payload, null, 2);
}

for (const id of ["dc-top", "dc-trigger", "dc-depth", "dc-probe"]) {
    $(id).addEventListener("input", updatePreview);
}

$("dc-close").addEventListener("click", () => window.close());

updatePreview();
void initWindow().catch(() => {});
