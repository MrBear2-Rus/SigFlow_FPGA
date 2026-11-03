> 负责人：js工
> 类型：规划
> 更新：2026-11-03
> 状态：进行中
> 阶段：V3

# js工 · 规划

## ① 当前目标

UI 框架（U1–U7）与 Agent 托管（J6b）已完成。下一阶段补齐**不依赖 C++** 的功能面：
约束/引脚、仿真、FPGA 串联、编辑器与画布增强、工程与安全。
已完成能力见 [history-js.md](history-js.md)，架构见 [README](../README.md)。

## ② 已完成

| 系列 | 内容 |
|---|---|
| J0–J5 | 任务链路、HTTP/SSE、用户/工程/文件、编辑器、画布 |
| U1–U7 | 六页面 UI 框架、浅色主题、伪停靠、三个独立窗口、断链与文档检查 |
| J6b | Agent 方案 A：每用户 sidecar、Gateway 兼容路由、`/api/agent/*` 代理、助手面板 |
| 仍阻塞 | 波形（C++ P7）、TraceBridge 运行时（P8）、flash（P4 剩余） |

## ③ 新阶段与验收

| 阶段 | 内容 | 验收 |
|---|---|---|
| V1 | 约束 / 引脚绑定 UI | ✅ 引脚表、筛选、信号绑定、`.cst` 读写、`validate cst` |
| V2 | 仿真工作流 UI | ✅ `sim.build` / `sim.run` 表单与任务页，产物（sim_main / wave.vcd）可见 |
| V3 | FPGA 串联与报告 | ⏸ 暂缓：本机无 yosys/nextpnr/gowin_pack，等工具环境 |
| V4 | 编辑器增强 | ✅ 多文件页签、查找替换、诊断行标记与跳转 |
| V5 | 画布增强 | ✅ 缩放/平移、框选多选、撤销/重做、标签与坐标编辑 |
| V6 | 工程与安全 | ✅ 文件重命名/移动/删除；会话持久化（token 哈希落盘） |
| V7 | 打磨与自动化 | ✅ 浏览器冒烟 12 项、401 统一跳转、网络错误提示、`web_smoke` 目标 |

## ④ 阶段要点

**V1 约束 / 引脚**

1. 数据源只用 `backend/static/target-profiles/*.json` 与 `*.pins.json`，不在前端硬编码引脚；
2. 右侧栏「引脚绑定」页签显示引脚表，支持按名称/功能过滤；
3. 绑定结果导出为 `.cst` 文本，写回工程文件；导入时解析并回填；
4. 调 `POST /api/projects/{id}/...` 的校验接口（需新增 Node 端点转 `validate cst`）。

**V2 仿真**

1. 参数以 `contracts/run-v1/simulation-draft.md` 为准（评审后冻结）；
2. 任务页复用 FPGA 工具的任务控制台组件；
3. 波形查看仍等 P7，本阶段只做任务与产物列表。

**V3 串联**

1. synth 产物路径 `.sigflow/agent/jobs/synth/<id>/artifacts/<file>` 作为 pnr 输入；
2. pnr 产物同理传给 pack；
3. 指标展示按 `nextpnr-metrics.schema.json` 草案实现，字段变化需同步。

## ⑤ 下一步

1. V3 待本机工具链就绪后恢复（synth→pnr→pack 串联与指标展示）。
2. 收敛评审意见（H-CPP-009 / 012 / 018 / 019 / 021，结论见 handoff ⑤）。
3. 引脚 `validate pins` 与波形查看按 C++ 进度接入。
4. 登录限流、CSRF、工程导入导出留待后续迭代。
5. 视觉逐像素校准需要原版截图。

## ⑥ 阻塞与依赖

- 波形 / TraceBridge / flash 依赖 C++ P7、P8 与 P4 剩余项；
- V3 的 synth→pnr→pack 串联依赖真实 yosys / nextpnr / gowin_pack，本机未安装，暂缓；
- 约束、仿真、指标接口仍是草案，V1–V3 的字段以评审结果为准。

## ⑦ 相关决策

D-17（每用户 Agent sidecar）、D-18（波形协议等 Bear2），其余见
[README](../README.md) 的「决策记录」。
