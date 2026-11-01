> 负责人：cpp工
> 类型：规划
> 更新：2026-11-01
> 状态：进行中
> 阶段：P3 本地完成，P4 待启动

# cpp工 · 规划

> 本文件由原 `task.md` 迁移整理，保留原阶段划分、验收标准与决策编号，未改变原意。

## ① 当前目标

在 Linux 上交付自包含的 `sigflow-tool`，由 Node 托管；先打通 synth，再按 P4–P9 扩展。
不得以"能 configure"或"CLI 返回占位结果"当作业务完成。

## ② 阶段与验收

| 阶段 | 内容 | 验收标准 | 状态 |
|---|---|---|---|
| P0 | 平台层、工具发现、SHA256、nlohmann | 构建 + 基础回归通过 | ✅ 完成 |
| P1 | 冻结 run 契约（schema + fixtures） | C++ 序列化与 fixture 一致；Node 能独立读取 | ✅ 完成 |
| P2 | 单 Job Runtime + CLI | 4/4 CTest；51 个真实 CLI 场景；失败无成功 result | ✅ 完成（本地） |
| P3 | Yosys synth provider | 真实 Yosys 最小 RTL + Node 联调 | 🟡 假工具已过；Linux 原生工具与 Node 未验收 |
| P4 | pnr / pack / sim / flash | synth→pnr→pack、sim.build→sim.run→VCD；flash 需确认与租约 | ⬜ 未开始 |
| P5 | target / validate | 板卡数据只读；约束校验反例覆盖 | ⬜ |
| P6 | structure / lint | tree-sitter 离线、去 wx、行列定位 | ⬜ |
| P7 | wave worker | open/close/signals/query、边界值、性能阈值 | ⬜ |
| P8 | 设备与 TraceBridge | 串口/FTDI/采集、fake transport、授权硬件 | ⬜ |
| P9 | 独立部署 | install 后可运行、无源码路径依赖、交接完整 | ⬜ |

## ③ 下一步

1. P4：先迁 `pnr-nextpnr`，与 synth 串成第一条多步链路。
2. 每个 provider 单独列参数 / 工具版本 / 依赖产物 / 输出校验，不共用一份模糊验收。
3. 补 P3：Linux 原生 Yosys 的最小 RTL 验收（环境依赖见 handoff-cpp H-CPP-006）。

## ④ 阻塞与依赖

- 真实 Node 联调依赖 js 侧实现（handoff-cpp H-CPP-001 ~ H-CPP-003）。
- `wave serve` 对外字段需要 js 侧给出 UI 需求后冻结（H-CPP-004）。
- flash 确认传递形式待定（H-CPP-005）。

## ⑤ 相关决策

- 已确认：D01–D09（旧契约沿用、snake_case 落盘、`--project-id`、相对 artifact 路径、
  执行请求快照、guardian 取消）。
- 待定：D10（wave 字段）、D11（其他命令 payload）、D12（限额与性能阈值）。
- 跨角色决策见 `../../README.md` 的「决策记录」。
