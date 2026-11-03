// =============================================================================
// 图标：三套 svg 资源，按组引用
//   ui        工具栏 / 活动栏 / 文件树
//   signal    信号与工程类型
//   components 元件库
// 清单由 assets/icons/manifest.json 提供（构建时生成）。
// =============================================================================

const BASES = {
    ui: "/assets/icons/ui/",
    signal: "/assets/icons/signal/",
    components: "/assets/icons/components/",
};

export function iconURL(group, name) {
    const base = BASES[group];
    if (!base) throw new Error(`未知图标组：${group}`);
    return `${base}${name.endsWith(".svg") ? name : `${name}.svg`}`;
}

export function iconEl(group, name, { size = 16, alt = "" } = {}) {
    const img = document.createElement("img");
    img.src = iconURL(group, name);
    img.width = size;
    img.height = size;
    img.alt = alt;
    img.className = size > 16 ? "icon icon-lg" : "icon";
    img.loading = "lazy";
    return img;
}

let manifestPromise = null;

/** @returns {Promise<{ui: string[], signal: string[], components: string[]}>} */
export function loadIconManifest() {
    manifestPromise ??= fetch("/assets/icons/manifest.json").then((response) => {
        if (!response.ok) throw new Error(`图标清单加载失败：${response.status}`);
        return response.json();
    });
    return manifestPromise;
}
