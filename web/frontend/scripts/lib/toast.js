// =============================================================================
// Toast：右下角轻提示
// =============================================================================

let host = null;

function ensureHost() {
    if (host) return host;
    host = document.createElement("div");
    host.className = "toast-host";
    document.body.append(host);
    return host;
}

/**
 * @param {string} message
 * @param {{type?: "info"|"success"|"error", timeout?: number}} [options]
 */
export function toast(message, { type = "info", timeout = 3000 } = {}) {
    const element = document.createElement("div");
    element.className = "toast";
    element.dataset.type = type;
    element.textContent = message;
    ensureHost().append(element);

    const remove = () => {
        element.style.opacity = "0";
        element.style.transition = "opacity .16s ease";
        setTimeout(() => element.remove(), 180);
    };
    const timer = setTimeout(remove, timeout);
    element.addEventListener("click", () => {
        clearTimeout(timer);
        remove();
    });
}
