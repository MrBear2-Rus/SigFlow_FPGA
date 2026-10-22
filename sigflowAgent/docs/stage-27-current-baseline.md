# 阶段 27：当前基线与动态预测闭环

日期：2026-10-20。基线 04648a9；本阶段只修改 sigflowAgent 下 U2 适配和验收代码，不修改 Core、Verilator 插件或 demo。

## 计划适配

当前 Gateway 已提供受控 testbench timeline 和 duration_ticks。U2 PlanStep 补齐显式 duration_ticks 参数，只接受规范十进制 uint64 字符串，范围 0–18446744073709551615；拒绝布尔、整数、符号、前导零、时间单位和溢出。PlanCard、审批摘要和实际执行保留原字符串，不把 timeout_ms 隐式换成仿真 tick。

## 受控动态场景

现有 closed_loop 增加开发验收选项 --dynamic-stimulus，要求同时选择 simulation、waves、predictions。在本轮自有工作目录创建并清理测试工程，不触碰已有工程或工作区外文件。

固定测试输入为同步高有效复位触发器：时钟半周期 5 tick，复位在 tick 12 释放，en=1、d 初始为 1，tick 30 将 d 改为 0，仿真至 tick 40。预期由该显式输入和 RTL 独立得出：复位释放后的首个上升沿 tick 15，q 应为 1。

测试先运行真实综合取得基线，再经正式 U2 宿主的审批/依赖门禁执行 build 和 run。长 build 只读观察后，后继 run 仍要求显式确认。读取认证、摘要和 Job/revision/snapshot 绑定的完整 VCD 窗口，最后通过 prediction_request/prediction_submit 验证先提交预期、后返回观察。预测不增加 Job、模型或提示等级，重放不重复执行。

测试输入使用当前受控解析器支持的逐行赋值和独立延时，不假设支持任意 Verilog testbench 语法，也不把团队自有样例当作教师已审阅的通用复位规则。

## 验收探针修正

实际运行曾出现 Windows WinError 10048；原验收探针每次轮询创建短连接。验收 HTTP 客户端现在复用已完整读取的连接，减少轮询连接开销；服务端空闲关闭时，只对 GET 有界重连一次；POST 响应丢失不自动重试。协议错误仍拒绝，关闭显式释放连接。相关 3 项真实 loopback 测试先失败后通过。该修改仅涉及验收探针，不作为系统端口错误根因已经消除的证据。

工具 Job 的终态与完整规范化报告可能存在短暂发布差，基线探针在原截止时间内只读等待 completeness=complete、产物和输入摘要齐备，不将 partial 改标为完整。所有最终证据门禁保持。

## 运行方法

先构建已有可选无界面目标（本机 build-main-msvcrt 已配置相应选项）：

```powershell
cmake --build build-main-msvcrt --target edu_u2_gateway -j 4
cd sigflowAgent
.\.venv\Scripts\python.exe -m sigflow_edu_agent.host.closed_loop `
  --gateway ../build-main-msvcrt/sigflowAgent/integration/edu_u2_gateway.exe `
  --scenario simulation --waves --predictions --dynamic-stimulus `
  --work-dir ../.superpowers/sdd/u2-completion `
  --output docs/dynamic-stage27.json --timeout 300
```

--work-dir 是操作者指定的既有测试父目录；应使用仓库内自有目录。本机工具环境仍仅作用于验收子进程，不修改已安装的 Verilator、系统网络或永久环境配置。无工具直接失败，不用 Mock 或 skip 代替。

## 结果与限制

[真实运行记录](dynamic-stage27.json)验证 Core/Yosys/Verilator、3 Job、0 真实模型调用、0–40 tick 精确窗口与 q 的 3 条变化记录，tick 15 的显式预期 1 与观察一致。状态最终 Completed/outcome_indeterminate，表示整个计划仍缺完整功能判定；单点预测匹配不等于整个电路正确或学生掌握概念。

该结果已补齐本地动态刺激和预测链，不代表八类真实 detector、真实教师审阅、原生 UI 或目标平台签收。本轮自动测试不调用真实 DeepSeek。

## 完整回归与网络异常记录

独立审查修复后，最终完整发现运行 658 项，失败/错误 0，用时 126.430 秒，使用普通 unittest 发现和原断言，无 socket 包装或测试重试。逐项成功 625，单项跳过 33；另有 2 条类级跳过记录覆盖 18 个未运行方法，共收集 676 方法。35 条跳过记录的原因仍为历史 UCAgent 实验和未提供的原始课程快照，不属于本轮新增跳过。本轮新增 27 项定向回归通过，开发评测 40/40，规则 37、Mock 3，真实模型调用 0。

独立审查发现两项 P2：旧 Job 新会话跨中断重排导致误建议，以及包含隐藏 producer 的摘要暴露盲评来源。均补充先失败后通过的回归并修复，复审通过，细节见阶段 25/26。最终真实动态三 Job 场景在这些修复后再次通过，不增加真实模型调用或结论范围。

此前数次全量运行出现 10048、10055 及其引发的 SERVICE_UNAVAILABLE。曾检查动态端口范围、连接数量、测试进程与可用内存，尝试测试进程隔离和诊断绑定；没有证据确认系统端口范围已耗尽，也未据此修改生产客户端。一次完整诊断运行采用原 unittest 发现及原断言，仅为 socket.connect/bind 添加无重试的观察计数和首次失败停止；653 项无失败，用时 140.729 秒，connect 2308 次、bind 201 次、对应 API 异常 0。随后普通全量 653 项无失败（139.300 秒），再经独立审查修复后最终 658 项及真实动态闭环均通过。该结果证明当前版本完整回归可通过，不证明先前系统资源异常已根治；失败批次摘要一并保留。

复现普通完整回归：在 sigflowAgent 目录使用 .venv/Scripts/python.exe -m unittest discover -s tests -v。Windows 加密配置测试需正常当前用户身份；临时目录可指向仓库内自有目录。若再次出现系统套接字错误，应保留首次失败和资源状态，不能据此改为跳过测试、自动重放写操作或修改系统网络设置。

最终回归、源码及二进制摘要见[验证汇总](verification-stage25-27.json)。后续交接以 [U2 核对表](u2-completion-status.md)为准；剩余外部验收不继续扩大到其他成员实现。
