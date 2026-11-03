// FPGA 工具窗口（U6）：Yosys / nextpnr / 打包 三个任务控制台

import { api } from "./lib/api.js";
import { initTabs } from "./lib/tabs.js";
import { toast } from "./lib/toast.js";

const $ = (id) => document.getElementById(id);
const params = new URLSearchParams(location.search);
const projectId = params.get("project");
let project = null;

initTabs();

function setHeader() {
    $("projectName").textContent = project?.name ?? projectId ?? "";
    if (projectId) {
        $("workspaceLink").href = `/pages/workspace.html?project=${encodeURIComponent(projectId)}`;
    }
    document.title = `${project?.name ?? "FPGA 工具"} · SigFlow`;
}

class JobConsole {
    constructor(host, { jobType, onArtifact, artifactAction }) {
        this.host = host;
        this.jobType = jobType;
        this.onArtifact = onArtifact;
        this.artifactAction = artifactAction ?? "使用产物";
        this.jobs = [];
        this.selected = null;
        this.source = null;
        this.#build();
    }

    #build() {
        this.host.textContent = "";
        const head = document.createElement("div");
        head.className = "job-console-head";
        const title = document.createElement("strong");
        title.textContent = "任务";
        this.count = document.createElement("span");
        this.count.className = "muted";
        const spacer = document.createElement("span");
        spacer.style.flex = "1";
        const refresh = document.createElement("button");
        refresh.type = "button";
        refresh.className = "btn btn-sm";
        refresh.textContent = "刷新";
        refresh.addEventListener("click", () => this.refresh());
        head.append(title, this.count, spacer, refresh);

        this.list = document.createElement("div");
        this.list.className = "job-list";

        this.log = document.createElement("div");
        this.log.className = "job-log";

        this.artifacts = document.createElement("div");
        this.artifacts.className = "job-artifacts";

        this.host.append(head, this.list, this.log, this.artifacts);
    }

    async refresh() {
        const data = await api.get("/api/jobs");
        this.jobs = (data.jobs ?? []).filter(
            (job) => job.projectId === projectId && job.jobType === this.jobType,
        );
        this.count.textContent = `${this.jobs.length} 个`;
        this.#renderList();
        if (this.jobs.length === 0) {
            this.log.textContent = "暂无任务";
            this.artifacts.textContent = "";
        }
    }

    #renderList() {
        this.list.textContent = "";
        for (const job of this.jobs) {
            const row = document.createElement("div");
            row.className = "job-row";
            row.setAttribute("aria-selected", String(this.selected === job.id));
            row.addEventListener("click", () => this.select(job.id));

            const id = document.createElement("span");
            id.className = "id";
            id.textContent = job.id.slice(0, 8);
            id.title = job.id;

            const time = document.createElement("span");
            time.className = "muted";
            time.textContent = new Date(job.createdAt).toLocaleTimeString("zh-CN", { hour12: false });

            const state = document.createElement("span");
            state.className = "state";
            state.dataset.state = job.state;
            state.textContent = job.state;

            row.append(id, time, state);
            if (job.state === "Running" || job.state === "Queued") {
                const cancel = document.createElement("button");
                cancel.type = "button";
                cancel.className = "btn btn-sm";
                cancel.textContent = "取消";
                cancel.addEventListener("click", async (event) => {
                    event.stopPropagation();
                    try {
                        await api.post(`/api/jobs/${job.id}/cancel`, {});
                        toast("已请求取消");
                        await this.refresh();
                    } catch (error) {
                        toast(`取消失败：${error.message}`, { type: "error" });
                    }
                });
                row.append(cancel);
            }
            this.list.append(row);
        }
    }

    async select(jobId) {
        this.selected = jobId;
        this.#renderList();
        this.log.textContent = "";
        this.artifacts.textContent = "";
        this.#closeStream();

        let detail;
        try {
            detail = await api.get(`/api/jobs/${jobId}`);
        } catch (error) {
            toast(`读取任务失败：${error.message}`, { type: "error" });
            return;
        }
        this.#append(`任务 ${detail.id}（${detail.jobType}）状态：${detail.state}`);
        if (detail.error) this.#append(`错误：${detail.error}`, "err");
        this.#renderArtifacts(detail);

        if (detail.state === "Queued" || detail.state === "Running") {
            this.#connect(jobId);
        }
    }

    #connect(jobId) {
        const source = new EventSource(`/api/jobs/${jobId}/events`);
        this.source = source;
        source.onmessage = (message) => {
            try {
                this.#renderEvent(JSON.parse(message.data));
            } catch {
                this.#append(message.data);
            }
        };
        source.addEventListener("end", async (message) => {
            try {
                const payload = JSON.parse(message.data || "{}");
                this.#append(`任务结束：${payload.state ?? "unknown"}`);
            } catch {
                this.#append("任务结束");
            }
            this.#closeStream();
            await this.refresh();
            await this.select(jobId);
        });
        source.onerror = () => {
            this.#append("事件流中断（浏览器会自动重连）", "warn");
        };
    }

    #closeStream() {
        if (this.source) {
            this.source.close();
            this.source = null;
        }
    }

    #renderEvent(event) {
        switch (event.type) {
            case "log":
                this.#append(`[${event.level}] ${event.line}`);
                break;
            case "progress":
                this.#append(`[进度] ${event.percent}% ${event.status ?? ""}`);
                break;
            case "diagnostic":
                this.#append(`[诊断] ${event.severity} ${event.code}: ${event.message}`, event.severity === "error" ? "err" : "warn");
                break;
            case "artifact":
                this.#append(`[产物] ${event.path}`);
                break;
            case "result":
                this.#append(`[结果] state=${event.state} exit=${event.exitCode}`);
                break;
            default:
                this.#append(JSON.stringify(event));
        }
    }

    #append(text, kind = "") {
        const line = document.createElement("span");
        line.className = `line ${kind}`;
        line.textContent = text;
        this.log.append(line, document.createElement("br"));
        this.log.scrollTop = this.log.scrollHeight;
    }

    #renderArtifacts(detail) {
        this.artifacts.textContent = "";
        for (const artifact of detail.artifacts ?? []) {
            const link = document.createElement("a");
            link.href = artifact.url;
            link.textContent = artifact.path.split("/").pop();
            link.title = artifact.path;
            this.artifacts.append(link);

            if (this.onArtifact) {
                const use = document.createElement("button");
                use.type = "button";
                use.className = "btn btn-sm";
                use.textContent = this.artifactAction;
                use.addEventListener("click", () => {
                    this.onArtifact(
                        `.sigflow/agent/jobs/${this.jobType}/${detail.id}/${artifact.path}`,
                        artifact,
                    );
                });
                this.artifacts.append(use);
            }
        }
    }

    async submit(jobParams) {
        const job = await api.post("/api/jobs", {
            projectId,
            jobType: this.jobType,
            params: jobParams,
        });
        await this.refresh();
        await this.select(job.id);
        return job;
    }
}

const synth = new JobConsole($("console-synth"), {
    jobType: "synth",
    onArtifact: (path) => {
        $("pnr-netlist").value = path;
        const tab = document.querySelector('[data-tabs] .tab[data-tab="pnr"]');
        tab?.click();
        toast("已填入网表路径");
    },
});
const pnr = new JobConsole($("console-pnr"), { jobType: "pnr" });
const pack = new JobConsole($("console-pack"), { jobType: "pack" });
const flash = new JobConsole($("console-flash"), { jobType: "flash" });
const simBuild = new JobConsole($("console-sim-build"), {
    jobType: "sim.build",
    artifactAction: "用作仿真程序",
    onArtifact: (path) => {
        $("sim-exe").value = path;
        toast("已填入仿真程序路径");
    },
});
const simRun = new JobConsole($("console-sim-run"), { jobType: "sim.run" });

function lines(id) {
    return $(id).value.split(/\r?\n/).map((line) => line.trim()).filter(Boolean);
}

$("synth-submit").addEventListener("click", async () => {
    try {
        await synth.submit({
            top_module: $("synth-top").value.trim(),
            source_files: lines("synth-sources"),
            strategy: $("synth-strategy").value,
            yosys_family: $("synth-family").value.trim() || "gw1n",
        });
    } catch (error) {
        toast(`提交失败：${error.message}`, { type: "error" });
    }
});

$("synth-refresh").addEventListener("click", () => synth.refresh());

$("pnr-submit").addEventListener("click", async () => {
    const body = {
        top_module: $("pnr-top").value.trim(),
        netlist: $("pnr-netlist").value.trim(),
        device: $("pnr-device").value.trim(),
        family: $("pnr-family").value.trim(),
    };
    const cst = $("pnr-cst").value.trim();
    if (cst) body.cst = cst;
    try {
        await pnr.submit(body);
    } catch (error) {
        toast(`提交失败：${error.message}`, { type: "error" });
    }
});

$("pnr-refresh").addEventListener("click", () => pnr.refresh());

$("pack-submit").addEventListener("click", async () => {
    const body = {
        pnr_json: $("pack-pnr").value.trim(),
        device: $("pack-device").value.trim(),
    };
    const output = $("pack-output").value.trim();
    if (output) body.output = output;
    try {
        await pack.submit(body);
    } catch (error) {
        toast(`提交失败：${error.message}`, { type: "error" });
    }
});

$("sim-build-submit").addEventListener("click", async () => {
    const body = {
        top_module: $("sim-top").value.trim(),
        source_files: lines("sim-sources"),
    };
    const testbench = $("sim-testbench").value.trim();
    if (testbench) body.testbench = testbench;
    try {
        await simBuild.submit(body);
    } catch (error) {
        toast(`提交失败：${error.message}`, { type: "error" });
    }
});

$("sim-build-refresh").addEventListener("click", () => simBuild.refresh());

$("sim-run-submit").addEventListener("click", async () => {
    const body = { sim_exe: $("sim-exe").value.trim() };
    const duration = $("sim-duration").value.trim();
    if (duration) body.duration_ticks = duration;
    if (!body.sim_exe) {
        toast("请先选择仿真程序", { type: "error" });
        return;
    }
    try {
        await simRun.submit(body);
    } catch (error) {
        toast(`提交失败：${error.message}`, { type: "error" });
    }
});

$("sim-run-refresh").addEventListener("click", () => simRun.refresh());

$("closeWindow").addEventListener("click", () => window.close());

async function boot() {
    try {
        await api.get("/api/session");
    } catch {
        location.href = "/";
        return;
    }
    if (!projectId) {
        toast("缺少 project 参数", { type: "error" });
        $("projectName").textContent = "未指定工程";
        return;
    }
    try {
        project = await api.get(`/api/projects/${encodeURIComponent(projectId)}`);
    } catch {
        toast("工程不存在", { type: "error" });
        return;
    }
    setHeader();

    // 用工程内的 Verilog 文件预填源文件
    try {
        const data = await api.get(`/api/projects/${encodeURIComponent(projectId)}/files`);
        const sources = (data.files ?? [])
            .filter((entry) => entry.type === "file" && /\.(v|sv)$/i.test(entry.path))
            .map((entry) => entry.path);
        if (sources.length > 0) {
            $("synth-sources").value = sources.join("\n");
            $("sim-sources").value = sources.join("\n");
        }
    } catch {
        /* 忽略 */
    }

    await synth.refresh();
    await pnr.refresh();
    await pack.refresh();
    await flash.refresh().catch(() => {});
    await simBuild.refresh();
    await simRun.refresh();
}

void boot();
