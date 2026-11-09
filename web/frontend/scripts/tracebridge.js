// TraceBridge 窗口：设备枚举 + 页签初始化 + 登录校验

import { api } from "./lib/api.js";
import { initTabs } from "./lib/tabs.js";
import { toast } from "./lib/toast.js";
import { initWindow } from "./window-common.js";

initTabs();

async function loadDevices() {
    const host = document.getElementById("deviceList");
    host.textContent = "枚举中…";
    try {
        const data = await api.get("/api/devices");
        host.textContent = "";
        const meta = document.createElement("p");
        meta.className = "muted small";
        const caps = data.capabilities ?? {};
        const ftdi = data.ftdi_probe ?? {};
        meta.textContent = `${data.platform ?? "?"} · 串口 ${caps.serial ? "可用" : "不可用"} · FTDI ${
            caps.ftdi ? (ftdi.device_present ? "已发现" : "已编译/未发现") : "未编译"
        }`;
        host.append(meta);
        const ports = data.serial_ports ?? [];
        if (ports.length === 0) {
            const empty = document.createElement("p");
            empty.className = "muted small";
            empty.textContent = "没有发现串口设备";
            host.append(empty);
        } else {
            const list = document.createElement("ul");
            list.className = "device-ports";
            for (const port of ports) {
                const item = document.createElement("li");
                const name = document.createElement("strong");
                name.textContent = port.name || port.path;
                const detail = document.createElement("span");
                detail.className = "muted small";
                detail.textContent = [
                    port.path,
                    port.vendor_id && port.product_id ? `${port.vendor_id}:${port.product_id}` : "",
                    port.usb_serial ?? "",
                ].filter(Boolean).join(" · ");
                item.append(name, detail);
                list.append(item);
            }
            host.append(list);
        }
    } catch (error) {
        host.textContent = `枚举失败：${error.message}`;
    }
}

document.getElementById("devicesRefresh").addEventListener("click", () => {
    loadDevices().catch((error) => toast(error.message, { type: "error" }));
});

void initWindow().then(() => loadDevices()).catch(() => {});
