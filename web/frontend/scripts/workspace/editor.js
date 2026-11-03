// CodeMirror 封装：打开/保存、跳转、脏标记、查找替换、诊断行标记

export class CodeEditor {
    constructor(host, { onChange, onCursor } = {}) {
        this.cm = CodeMirror(host, {
            mode: "text/x-verilog",
            lineNumbers: true,
            matchBrackets: true,
            indentUnit: 4,
            tabSize: 4,
            lineWrapping: false,
        });
        this.cm.setSize("100%", "100%");
        this.dirty = false;
        this.marks = [];
        this.cm.on("change", () => {
            if (!this.loading) this.dirty = true;
            onChange?.(this.dirty);
        });
        this.cm.on("cursorActivity", () => {
            const cursor = this.cm.getCursor();
            onCursor?.({ line: cursor.line + 1, ch: cursor.ch + 1 });
        });
    }

    open(content) {
        this.loading = true;
        this.cm.setValue(content ?? "");
        this.cm.clearHistory();
        this.cm.setCursor({ line: 0, ch: 0 });
        this.loading = false;
        this.dirty = false;
        this.setDiagnostics([]);
    }

    getValue() {
        return this.cm.getValue();
    }

    goto(line, col = 1) {
        const target = { line: Math.max(0, line - 1), ch: Math.max(0, col - 1) };
        this.cm.setCursor(target);
        this.cm.scrollIntoView(target, 80);
        this.cm.focus();
    }

    refresh() {
        this.cm.refresh();
    }

    get lineCount() {
        return this.cm.lineCount();
    }

    get cursor() {
        const cursor = this.cm.getCursor();
        return { line: cursor.line + 1, ch: cursor.ch + 1 };
    }

    /** 在行号栏与行背景上标出 lint 诊断。 */
    setDiagnostics(diagnostics) {
        for (const mark of this.marks) {
            this.cm.removeLineClass(mark.handle, "background", mark.cls);
            this.cm.setGutterMarker(mark.handle, "CodeMirror-linenumbers", null);
        }
        this.marks = [];
        for (const item of diagnostics ?? []) {
            if (!item.line) continue;
            const handle = this.cm.getLineHandle(item.line - 1);
            if (!handle) continue;
            const cls = item.severity === "error" ? "cm-diag-error" : "cm-diag-warning";
            this.cm.addLineClass(handle, "background", cls);
            const marker = document.createElement("div");
            marker.className = `cm-diag-marker ${item.severity}`;
            marker.title = item.message ?? "";
            marker.textContent = item.severity === "error" ? "●" : "▲";
            this.cm.setGutterMarker(handle, "CodeMirror-linenumbers", marker);
            this.marks.push({ handle, cls });
        }
    }

    /** 查找下一个/上一个；返回是否命中。 */
    find(query, { back = false } = {}) {
        if (!query) return false;
        const text = this.cm.getValue();
        const escaped = query.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
        const pattern = new RegExp(escaped, "gi");
        const matches = [];
        let match;
        while ((match = pattern.exec(text)) !== null) {
            matches.push({ start: match.index, end: match.index + match[0].length });
            if (match[0].length === 0) pattern.lastIndex += 1;
        }
        if (matches.length === 0) return false;

        const offset = this.cm.indexFromPos(this.cm.getCursor());
        const next = matches.findIndex((item) => item.start >= offset);
        let chosen;
        if (back) {
            chosen = next <= 0 ? matches[matches.length - 1] : matches[next - 1];
        } else {
            chosen = next === -1 ? matches[0] : matches[next];
        }
        this.cm.setSelection(
            this.cm.posFromIndex(chosen.start),
            this.cm.posFromIndex(chosen.end),
        );
        this.cm.scrollIntoView(this.cm.getCursor(), 80);
        this.cm.focus();
        return true;
    }

    /** 用新文本替换当前选区（若选区为空，先查找）。 */
    replace(query, replacement) {
        const selected = this.cm.getSelection();
        if (!selected || (query && selected.toLowerCase() !== query.toLowerCase())) {
            if (!this.find(query)) return false;
        }
        this.cm.replaceSelection(replacement ?? "");
        return true;
    }

    /** 全部替换；返回替换次数。 */
    replaceAll(query, replacement) {
        if (!query) return 0;
        const text = this.cm.getValue();
        const pattern = new RegExp(query.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "gi");
        let count = 0;
        const next = text.replace(pattern, () => {
            count += 1;
            return replacement ?? "";
        });
        if (count > 0) {
            this.loading = true;
            this.cm.setValue(next);
            this.loading = false;
            this.dirty = true;
        }
        return count;
    }
}
