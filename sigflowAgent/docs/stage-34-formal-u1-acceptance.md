# 阶段 34：正式 U1 宿主与真实工程联合验证

日期：2026-10-21；基线 04648a9。入口为 `sigflow_u2.host.shared_acceptance`，使用现有 headless Core/Gateway 测试宿主和真实 `agent` 主程序，启用 `--u2-teaching`。U1/U2 共用一个教学服务端口，Gateway 保持自身认证端口；不启动另一个 U2 HTTP sidecar，不恢复 GUI demo。

## 执行范围

验证入口只在自己的 TemporaryDirectory 中创建工程和刺激，结束后清理自己持有的目录。默认 simulation 场景执行 Yosys 基线、Verilator build 和 run 共 3 个真实 Job；synthesis 场景执行 2 个真实 Yosys Job。两个场景都通过正式 U1 教学扩展完成以下步骤：

1. 创建外部 session/run，重放复用原标识；未认证请求拒绝。
2. 当前工程证据生成 L1，显式 hint_next 升 L2，同动作重放不再推进、不启动 Job。
3. 关闭并以相同 instance/data_directory 重启 U1，原外部标识可读取，但旧卡隔离；显式 recover 重新取证后保留 L2，再明确升级 L3 并 finish。
4. 教学验证先等待审批；只有真实 UI grant 才执行。长 Job 经只读 GET 等待、显式 observe_verification 取结果；多步骤计划必须另外确认后继，不能自动启动。
5. 校验反馈使用新 Job 的真实工程证据；共享 agent-run 只在教学 Completed 后返回，规则模式为 degraded。
6. 由测试父进程编辑自己的 RTL、要求 Core 刷新后，旧版本读取/动作拒绝；取消仍可被接受，不新增 Job。最新 simulation 还验证只含外部身份、序号、版本、动作和状态的事件元数据；较早的 synthesis 报告不包含这条新增断言。

simulation 还通过同一正式入口执行 prediction_request、接收未含观察值的 challenge，再显式提交 tick 15 的独立预期 1。真实 VCD 精确窗口为 0–40 tick，结果比较不增加 Job、模型调用或提示等级。比较结束须显式 finish，释放工程活动会话后再创建取消场景。这里的预期来自测试工程明确声明的刺激，不是从观察值倒推。

两个入口固定规则模式，屏蔽继承的模型配置，真实模型调用为 0。课程背景继续通过原工厂提供，不修改 U3 数据。缺工具、证据、U1 源码、清理或报告写入失败时返回失败，不跳过为成功；原始子进程输出、问题、凭据和路径不进入报告。

## 复现命令

在 sigflowAgent 目录执行；命令中的工作目录由操作者提前创建并持有。

```powershell
.\.venv\Scripts\python.exe -m sigflow_u2.host.shared_acceptance --gateway ..\build-main-msvcrt\sigflowAgent\integration\edu_u2_gateway.exe --scenario simulation --work-dir ..\.superpowers\sdd\u1-u2-stage32-34-plan --output docs\shared-stage34-simulation.json --timeout 300
```

需要较轻的综合链路时，将 scenario 改为 synthesis 并使用单独输出文件。`--u1-source` 可由可信操作者明确指定 U1 源码根；默认使用仓库 agent/src。正式安装 U1 和 U2 时分别使用独立分发，不能通过普通请求指定导入路径。

## 故障与修正记录

最初真实综合验收定位到测试 UI 通道跨教学/重启轮次后闲置过期。入口在签发新的 grant 之前主动建立新连接，每个写操作仍只有一次提交，不重试结果未知的 POST。综合复测通过。

最初仿真验收已完成预测比较，随后新建取消会话时得到 409：预测会话仍为 WaitingStudent，触发现有 project_busy。入口改为显式结束预测会话后再开始下一场景；没有放宽并发规则或版本检查。409 的现场与失败报告保留在本计划的工作区日志中。

## 交付与验收边界

最终完整 U1/U2 回归、开发评测、真实结果、源码/二进制摘要及审查结果见[阶段验证](verification-stage32-34.json)、[综合报告](shared-stage34-synthesis.json)和[仿真报告](shared-stage34-simulation.json)。原阶段 28–31 报告保持历史快照，不覆盖为本阶段数据。

本阶段证明已启用的正式 U1 教学扩展能连接 U2 和真实工程证据。原 `/api/v1/sessions` 与 `/runs` 基础语义保持，原生 GUI 客户端尚未消费 `/api/v1/u2`，不能宣称主界面已完成该交互。真实 UA-10 接收端、真实审阅材料/教师权限、额外 detector/行为契约、冻结模型集、真实重复模型批次、双教师评审以及目标平台/UI 签收仍是独立外部条件。工具执行和单个采样点 matches 不代表整个设计功能正确或教学质量通过。
