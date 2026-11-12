> 负责人：js工
> 类型：规划
> 更新：2026-11-08
> 状态：进行中
> 阶段：V12

# js工 · 规划

## ① 当前目标

C++ 已交付 `wave serve`（P7）与 TraceBridge 地基（P8，capture 无 CLI）。下一阶段：
**接入波形**（每用户 worker + `/api/wave/*` + 波形面板），评审 devices/capture 草案；
V3（FPGA 串联）仍因本机缺 yosys/nextpnr/gowin_pack 暂缓。

已完成能力见 [history-js.md](history-js.md)，架构见 [README](../README.md)，
C++ 侧情况见 [history-cpp.md](history-cpp.md)。

## ② 已完成

| 系列 | 内容 |
|---|---|
| J0–J5 | 任务链路、HTTP/SSE、用户/工程/文件、编辑器、画布 |
| U1–U7 | 六页面 UI 框架、浅色主题、伪停靠、三个独立窗口、断链与文档检查 |
| J6b | Agent 方案 A：每用户 sidecar、Gateway、代理、助手面板 |
| V1–V7 | 约束/引脚、仿真、编辑器、画布、文件与会话、浏览器冒烟 |
| 评审 | analysis / target / nextpnr / pack / simulation 五份草案已回意见 |

## ③ 新阶段与验收

| 阶段 | 内容 | 验收 |
|---|---|---|
| V8 | 波形接入 | ✅ 每用户 worker、产物扫描、`/api/wave/*`；波形页签可列信号、查询并绘制（游标/缩放留 V10） |
| V9 | 设备与 TraceBridge | ✅ `/api/devices` + TraceBridge 设备列表；capture 评审已回（待 CLI/租约） |
| V10 | 收尾（一） | ✅ 波形游标/缩放、CSRF 同源检查、登录限流 |
| V11 | 收尾（二） | ✅ 工程导入导出（JSON bundle）；Node 22 ✅（28/28） |
| V12 | 契约回执 | ✅ 五份契约确认；wave 新错误码映射与 E2E；D-18 冻结 |
| V3 | FPGA 串联与指标 | ⏸ 等本机 yosys / nextpnr / gowin_pack |

## ④ 阶段要点

**V8 波形**

1. `src/wave/worker.js`：每用户一个 `sigflow-tool wave serve`，JSONL 按 `id` 关联；
   请求 ≤64 KiB、响应 ≤900 KiB，超限报错而不是截断；崩溃按退避重启，空闲回收。
2. 项目切换：先 `close` 再 `open` 新 VCD；`open` 失败保留旧 trace（与草案一致）。
3. 时间戳与 `begin/end` 全按字符串传输；`query` 一次只查一个 `signal_id`，
   前端多信号就发多次请求，避免自行发明批量协议。
4. HTTP：`GET /api/wave/signals`、`POST /api/wave/query`；VCD 只允许来自本工程
   `sim.run` 产物，路径经 ProjectStore 校验。
5. 前端：底部「波形」页签替换空态；信号树、时间窗、游标、翻页；渲染用 SVG。

**V9 设备**

1. `GET /api/devices` 转 `sigflow-tool devices list`；TraceBridge 页展示端口与 FTDI 能力。
2. capture 只做契约与租约设计：`device_id` + Node 设备表串行化，等 C++ 接 CLI 再落地。
3. 不把 `ftdi_probe.device_present=null` 当成“没有设备”，要与 `false` 区分显示。

**V10 收尾**

波形游标/缩放、CSRF 与登录限流、工程导入导出、Node 22 复验均已完成；契约收敛待 cpp 回复。

## ⑤ 下一步

1. P8 按 D-20 做虚拟板卡规划（H-CPP-028），JS 参与接口与验收设计。
2. capture/硬件采集按 D-19/D-20 暂缓；devices 保留只读。
3. V3 等工具链；H-CPP-007/011/015 随对应功能补测试与字段。
4. 上线前补 HTTPS 与注册策略。
5. 引脚页已按新数据复验（88 脚、长 IO 描述省略、CST 校验通过）。

## ⑥ 阻塞与依赖

- V3 需要 yosys / nextpnr-gowin / gowin_pack；
- capture 需要 C++ 接 CLI/JSONL 并完成 D11 与设备租约；
- 波形真实数据依赖 `sim.run` 产物与真实 Verilator（已有）；
- Node 22 环境本机暂无，需临时下载或由集成方执行；
- 逐像素视觉校准仍缺原版截图。

## ⑦ 相关决策

D-17（每用户 Agent sidecar）、D-18（波形协议等 Bear2 草案——草案已到，进入评审），
其余见 [README](../README.md) 的「决策记录」。
