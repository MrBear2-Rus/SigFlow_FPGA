// =============================================================================
// 弹窗：基于原生 <dialog>，提供 confirm 与自定义动作
// =============================================================================

/**
 * @param {{
 *   title?: string,
 *   content?: string | Node,
 *   actions?: Array<{
 *     label: string,
 *     value: unknown,
 *     primary?: boolean,
 *     danger?: boolean,
 *     onClick?: () => boolean | Promise<boolean>,
 *   }>,
 *   width?: number,
 * }} options
 * @returns {Promise<unknown>} 选中动作的 value；关闭返回 null
 */
export function showDialog({ title = "", content = "", actions = [], width = 0 } = {}) {
    return new Promise((resolve) => {
        const dialog = document.createElement("dialog");
        dialog.className = "dlg";
        if (width > 0) dialog.style.width = `min(${width}px, calc(100vw - 32px))`;

        const head = document.createElement("div");
        head.className = "dlg-head";
        head.textContent = title;

        const body = document.createElement("div");
        body.className = "dlg-body";
        if (typeof content === "string") body.textContent = content;
        else if (content instanceof Node) body.append(content);

        const foot = document.createElement("div");
        foot.className = "dlg-foot";

        let settled = false;
        const finish = (value) => {
            if (settled) return;
            settled = true;
            dialog.close();
            dialog.remove();
            resolve(value);
        };

        for (const action of actions) {
            const button = document.createElement("button");
            button.className = "btn";
            if (action.primary) button.classList.add("btn-primary");
            if (action.danger) button.classList.add("btn-danger");
            button.textContent = action.label ?? "确定";
            button.addEventListener("click", async () => {
                if (action.onClick) {
                    button.disabled = true;
                    try {
                        const proceed = await action.onClick();
                        if (proceed === false) return;
                    } finally {
                        button.disabled = false;
                    }
                }
                finish(action.value);
            });
            foot.append(button);
        }
        if (actions.length === 0) {
            const button = document.createElement("button");
            button.className = "btn";
            button.textContent = "关闭";
            button.addEventListener("click", () => finish(null));
            foot.append(button);
        }

        dialog.append(head, body, foot);
        dialog.addEventListener("close", () => {
            if (!settled) finish(null);
        });
        document.body.append(dialog);
        dialog.showModal();
    });
}

export async function confirmDialog({
    title = "确认",
    message = "",
    confirmText = "确定",
    cancelText = "取消",
    danger = false,
} = {}) {
    const value = await showDialog({
        title,
        content: message,
        actions: [
            { label: cancelText, value: false },
            { label: confirmText, value: true, primary: !danger, danger },
        ],
    });
    return value === true;
}
