# U2 阶段 16：真实仿真执行链实施计划

日期：2026-10-16。承接阶段 15 的再规划与持续开发授权；本阶段先验收真实 Verilator 构建/运行和教学反馈，再单独推进精确波形。使用 writing-plans 与测试先行流程执行。

## 设计与选择

采用现有可选 CMake 测试宿主、真实插件、Gateway grant 和常驻教学宿主。在闭环运行器增加显式 simulation 场景，复用已验证的握手、隔离、超时和清理。独立新增整个运行器会重复进程安全逻辑；直接解除精确波形门禁会绕过尚未修复的 S2 完整性缺陷，因此本阶段只核验真实 VCD 产物的归属/hash/schema，不解释其采样正确性。

数据流为真实 Yosys 基线 → WaitingApproval → UI 对 build/run 计划审批 → Verilator build → 只读观察（如需）→ 后继显式再次确认 → run → 新报告与 VCD 元数据 → 中性教学反馈。若 build 在同步等待内完成，原有审批允许依赖顺序执行；若转为只读续查，则不得自动启动 run，需再次 execute_plan。每个步骤 grant 仅消费一次。

当前官方插件生成的 harness 仅调用一次 eval/dump，不驱动时钟或时间推进。报告明确标记 single_evaluation、功能正确性 not_evaluated；不能据此宣称动态仿真、预测或复位教学已验收。

## 约束

- Python 服务、测试、实现文档和可选宿主均在 sigflowAgent；保留旧 agent 与现有未提交改动、stash。
- 不暂存、不提交、不推送，不清理无关或沙箱外文件。只清理本运行器自己创建的临时工程。
- 默认 Yosys / 课程 / 跨版本场景保持原行为。仿真仅由可信运行器启动参数选择；不增加教学 HTTP 的环境、路径或工具注入入口。
- Windows 使用仓库已有 Verilator 与 MinGW、Git 附带的 sh/Perl 和实际 Python；只在本次临时目录放 make.exe 别名，不改全局环境、工具文件或第三方插件。
- 工具缺失、失败、报告不完整、产物绑定不符、关闭或落盘失败均返回失败，不使用 fixture 或 skip 冒充真实通过。
- 不调用 DeepSeek；L4 和真实精确波形门禁不变，U3 数据库与算法不改。

## Task 1：仿真组装与工具环境

文件：integration/edu_u2_gateway.cpp、integration/CMakeLists.txt；新增 host/simulation_acceptance.py、tests/test_simulation_acceptance.py。

- [x] 新测试先断言缺少仿真工具准备入口，覆盖本地 make 别名、限定子进程环境、缺工具固定失败码及源码工具不变。
- [x] `simulation_environment(directory, *, repo_root=None, inherited=None)` 返回子进程环境；Windows 将仓库 make 复制到独立目录，加入同一编译器及 Git sh 目录，指定 Verilator 根和真实基础 Python，不执行 shell 拼接命令。
- [x] 宿主 bootstrap 增加可选布尔 simulation，默认 false；只在显式启用时强链接并注册真实 build/run 插件、验证工具可运行；默认综合流程无仿真依赖。
- [x] 构建现有 edu_u2_gateway 目标；通过真实场景而非文本断言验证插件是否可用。

实际工具探测后的修订：本机安装的 verilated.mk 保留了打包机器绝对工具路径，增加仅供子进程的 MAKEFLAGS 覆盖与真实 make 回归；不修改第三方安装。真实构建报告另暴露产物未声明摘要的缺口：最小修改 InnerPlugin/eda-sim-verilator/src/VerilatorSimulator.cpp 与 VerilatorRunJob.cpp，在注册产物前计算 SHA-256、失败则拒绝。新增 integration/sim_artifact_test.cpp 和显式目标，Fake IProcessHost 回归在修复前两项 hash 断言均失败，修复后均通过，使用自建目录且无删除操作。

## Task 2：两步教学仿真与产物验收

文件：host/closed_loop.py、新 host/simulation_acceptance.py、tests/test_closed_loop.py、tests/test_simulation_acceptance.py。

审查后扩展 tests/test_simulation_transition.py：构造故意在 observe_verification 后返回 Completed 并自动启动 run 的 HTTP 边界 fixture。旧验收逻辑误通过，新增断言修复后拒绝该转换；正确的观察→WaitingApproval→显式执行路径继续通过。

- [x] 测试先行：CLI 接受 simulation，显式拒绝其 DeepSeek/课程组合；旧场景不注入新启动参数；失败保持脱敏。
- [x] 新 `exercise_simulation(gateway, ui, teaching, route, scope, baseline, deadline)` 构造固定 build/run 计划。验证能力/插件、审批前零新 Job、错误依赖拒绝且不消费 grant、执行中只读观察与后继再次确认、已消费步骤不重复执行。
- [x] 校验真实 build/run Job 的 project/revision/snapshot/capability/plugin、完整 Core 报告、退出码、不同 Job 和新 evidence；VCD 元数据匹配同一 run 与报告 hash/schema。report 只保留执行事实、产物 hash/ID，标记时间推进未覆盖。
- [x] 新增元数据篡改/缺失回归；运行全量 unittest、40 个开发评测、真实仿真场景与既有 Yosys+课程回归。阶段独立审查按实际缺陷修复并复验。
- [x] 产出 stage-16-simulation.md、闭环 JSON、源码摘要和验收记录，更新 README/task.md；检查文档与本轮未暂存状态。

## 验证命令

```powershell
$env:PYTHONPATH=(Resolve-Path sigflowAgent).Path
& sigflowAgent/.venv/Scripts/python.exe -P -m unittest discover -s sigflowAgent/tests -p test_simulation_acceptance.py -v
& sigflowAgent/.venv/Scripts/python.exe -P -m unittest discover -s sigflowAgent/tests -p test_closed_loop.py -v
cmake --build build-main-msvcrt --target edu_u2_gateway -j 4
& sigflowAgent/.venv/Scripts/python.exe -P -m sigflow_edu_agent.host.closed_loop --gateway build-main-msvcrt/sigflowAgent/integration/edu_u2_gateway.exe --scenario simulation --mode rule --timeout 300 --output sigflowAgent/docs/closed-loop-stage16-simulation.json
```

后续精确波形阶段须先修复并测试 S2 的末信号截断语义，再验收可信查询、学生预测动作及恢复绑定。真实动态 stimuli、GUI 和教师质量独立验收。

## 最终执行记录

新增模块测试 5 项先失败，CLI simulation 选择先报不支持；实现后通过。真实 make 路径覆盖用例先失败，私有 MAKEFLAGS 修复后通过。真实工具构建成功后暴露缺产物摘要，C++ 生产方回归先 exit=1（两项摘要均失败），插件最小补齐后 exit=0（两项均通过）。审查复现 observe 后自动执行后继和缺失进度字段的验收绕过，两个失败回归及异常字段子例修复后通过，独立复核 PASS。

最终构建成功；9 项仿真定向与 11 项 CLI/运行器回归覆盖当前场景。全量 unittest 运行 514 项、逐项成功 481、单项跳过 33，另有 2 个类级跳过覆盖 18 个未运行方法，总跳过记录 35，无失败，72.904 秒。开发评测 40/40，源码摘要匹配。最终真实场景 3 Job、1 次观察/1 次后继确认、394 字节 VCD 元数据核验；既有课程场景另通过 2 Job/4 背景，模型调用均为 0。见[开发手册](stage-16-simulation.md)与[机器验收汇总](verification-stage16.json)。不暂存、不提交、不推送。
