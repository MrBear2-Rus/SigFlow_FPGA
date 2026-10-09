# Part 2–3 验证记录

复核日期：2026-10-08。

## 基线

| 项目 | 版本或范围 |
|---|---|
| SigFlow | `SigFlow_FPGA_CMake`，`b7e5186` |
| UCAgent | `main`，`ac00768` |
| 执行环境 | Windows、PowerShell、项目 `.venv` |
| 模型与 EDA | 离线 fixture；未调用真实 DeepSeek 或 EDA 工具 |

SigFlow 经现有 GitHub 代理获取后快进更新，origin 配置保持不变。上游 UCAgent 已执行 fetch，并核对本地与 origin/main 无差异。原有暂存删除及 sigflowAgent 未提交内容保留。

## 验证命令

从 sigflowAgent 目录执行：

```powershell
$env:SIGFLOW_UCAGENT_SOURCE = (Resolve-Path '..\..\UCAgent\UCAgent').Path
.\.venv\Scripts\python.exe -m unittest discover -s tests -q
.\.venv\Scripts\python.exe -m unittest discover -s tests/integration -v
.\.venv\Scripts\python.exe -m sigflow_edu_agent.runtime.smoke
.\.venv\Scripts\python.exe -m pip check
.\.venv\Scripts\python.exe -m compileall -q sigflow_edu_agent tests
```

必须设置 SIGFLOW_UCAGENT_SOURCE；跳过上游集成测试的结果不能作为 Part 2 通过证据。

## 实测结果

| 检查 | 结果 |
|---|---|
| 全部测试 | 60 项通过，包含 6 项上游集成测试，无跳过 |
| 单独运行上游集成测试 | 6 项通过 |
| 真实阶段 smoke | Completed；E0–E4 共 5 次转换，state_version 从 0 推进至 5 |
| pip check | No broken requirements found |
| compileall | 成功，退出码 0 |
| 文档静态审查 | 9 份 Markdown；相对文件链接、围栏、Python 示例语法、对话式措辞检查通过 |

文档示例中由宿主提供的 loop、workspace、report、anchors 等变量属于嵌入片段，不是可独立启动的脚本；完整可运行命令由 smoke、probe 及测试入口提供。未验证外部网页链接的在线可用性。

## 覆盖范围

- Part 2：会话串行推进、版本冲突、状态隔离、等待态不推进、模型工具参数白名单；真实 VerifyStage、Checker、UCTool 生命周期和 E0–E4 完成轨迹；可信宿主授权后 E5/E6/E7 转换。
- Part 3：本地 HTTP 测试服务验证 health/capabilities 请求与鉴权头、协议/实例隔离、禁用能力与错误脱敏；读取共享 golden fixtures 验证指定 Job/诊断/产物绑定、混合版本拒绝、引用过期和上下文预算。
- 文档：相对文件链接、代码围栏、Python 代码块语法、命令可执行性及面向开发的措辞。

## 修复记录

E5 原判定只允许 Running，导致学生显式 finish 已进入 Completed 时 Checker 仍拒绝完成。新增回归先复现该失败，再放行可信宿主 finish 对应的 Completed。取消、失败、预算耗尽及过时阶段仍不能通过；finish 后 E4 旧阶段检查不放行。

独立定向审查覆盖上述 E5 条件修改及宿主动作约束，未发现可见授权绕过或阶段越过问题。审查依据为提交给审查者的精确条件与动作语义，不代表全目录独立源码审查或独立测试执行；完整测试由主执行环境运行。

## 验收边界

真实 UCAgent 类测试与独立阶段 smoke 已覆盖，尚不等同于完整 VerifyAgent 教育 profile 或 StageManager 调度集成。HTTP 测试服务不是 C++ Gateway；共享 OpenAPI 已扩展，但本客户端仅覆盖两个只读路由。

完整 Gateway 联调、真实报告获取、EDA 执行、L4 receipt、持久化恢复、跨进程幂等、真实模型教学质量及 Linux 平台仍需单独验收。当前结果不代表周期四 G0/G1 整体通过。
